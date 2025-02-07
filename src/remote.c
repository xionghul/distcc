/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003, 2004 by Martin Pool
 * Copyright 2007 Google Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

/*
 * Send a compilation request to a remote server.
 */


#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/time.h>

#include "distcc.h"
#include "trace.h"
#include "rpc.h"
#include "exitcode.h"
#include "util.h"
#include "clinet.h"
#include "hosts.h"
#include "exec.h"
#include "lock.h"
#include "compile.h"
#include "bulk.h"
#ifdef HAVE_GSSAPI
#include "auth.h"

/* Global security context in case confidentiality/integrity */
/* type services are needed in the future. */
extern gss_ctx_id_t distcc_ctx_handle;
#endif

/*
 * TODO: If cpp finishes early and fails then perhaps break out of
 * trying to connect.
 *
 * TODO: If we abort, perhaps kill the SSH child rather than closing
 * the socket.  Closing while a lot of stuff has been written through
 * might make us block until the other side reads all the data.
 */

/**
 * Open a connection using either a TCP socket or SSH.  Return input
 * and output file descriptors (which may or may not be different.)
 **/
static int dcc_remote_connect(struct dcc_hostdef *host,
                              int *to_net_fd,
                              int *from_net_fd,
                              pid_t *ssh_pid)
{
    int ret;

    if (host->mode == DCC_MODE_TCP) {
        *ssh_pid = 0;
        if ((ret = dcc_connect_by_name(host->hostname, host->port,
                                       to_net_fd)) != 0)
            return ret;
        *from_net_fd = *to_net_fd;
        return 0;
    } else if (host->mode == DCC_MODE_SSH) {
        if ((ret = dcc_ssh_connect(NULL, host->user, host->hostname,
                                   host->ssh_command,
                                   from_net_fd, to_net_fd,
                                   ssh_pid)))
            return ret;
        return 0;
    } else {
        rs_log_crit("impossible host mode");
        return EXIT_DISTCC_FAILED;
    }
}


static int dcc_wait_for_cpp(pid_t cpp_pid,
                            int *status,
                            const char *input_fname)
{
    int ret;

    if (cpp_pid) {
        dcc_note_state(DCC_PHASE_CPP, NULL, NULL, DCC_LOCAL);
        /* Wait for cpp to finish (if not already done), check the
         * result, then send the .i file */

        if ((ret = dcc_collect_child("cpp", cpp_pid, status, timeout_null_fd)))
            return ret;

        /* Although cpp failed, there is no need to try running the command
         * locally, because we'd presumably get the same result.  Therefore
         * critique the command and log a message and return an indication
         * that compilation is complete. */
        if (dcc_critique_status(*status, "cpp", input_fname, dcc_hostdef_local, 0))
            return 0;
    }
    return 0;
}


/* Send a request across to the already-open server.
 *
 * CPP_PID is the PID of the preprocessor running in the background.
 * We wait for it to complete before reading its output.
 */
static int
dcc_send_header(int net_fd,
                char **argv,
                struct dcc_hostdef *host)
{
    int ret;

    tcp_cork_sock(net_fd, 1);

    if ((ret = dcc_x_req_header(net_fd, host->protover)))
        return ret;
    if (host->cpp_where == DCC_CPP_ON_SERVER) {
        if ((ret = dcc_x_cwd(net_fd)))
            return ret;
    }
    if ((ret = dcc_x_argv(net_fd, "ARGC", "ARGV", argv)))
        return ret;

    return 0;
}

static char *
mangle_path (char const *base)
{
  /* Convert '/' to '#', convert '..' to '^'.  */
  const char *probe;
  char *buffer = (char *)malloc (strlen (base) + 1);
  char *ptr = buffer;

  for (; *base; base = probe)
    {
      size_t len;

      for (probe = base; *probe; probe++)
	if (*probe == '/')
	  break;
      len = probe - base;
      if (len == 2 && base[0] == '.' && base[1] == '.')
	*ptr++ = '^';
      else if (len == 1 && base[0] == '.' && base[1] == '/')
	{
	  probe++;
	  continue;
	}
      else
	{
	  memcpy (ptr, base, len);
	  ptr += len;
	}
      if (*probe)
	{
	  *ptr++ = '#';
	  probe++;
	}
    }

  /* Terminate the string.  */
  *ptr = '\0';

  return buffer;
}

static int max (int a, int b) {
    if (a>b)
      return a;
    else
      return b;
}

/**
 * Pass a compilation across the network.
 *
 * When this function is called, the preprocessor has already been
 * started in the background.  It may have already completed, or it
 * may still be running.  The goal is that preprocessing will overlap
 * with setting up the network connection, which may take some time
 * but little CPU.
 *
 * If this function fails, compilation will be retried on the local
 * machine.
 *
 * @param argv Compiler command to run.
 *
 * @param cpp_fname Filename of preprocessed source.  May not be complete yet,
 * depending on @p cpp_pid.
 *
 * @param files If we are doing preprocessing on the server, the names of
 * all the files needed; otherwise, NULL.
 *
 * @param output_fname File that the object code should be delivered to.
 *
 * @param cpp_pid If nonzero, the pid of the preprocessor.  Must be
 * allowed to complete before we send the input file.
 *
 * @param local_cpu_lock_fd If != -1, file descriptor for the lock file.
 * Should be != -1 iff (host->cpp_where != DCC_CPP_ON_SERVER).
 * If != -1, the lock must be held on entry to this function,
 * and THIS FUNCTION WILL RELEASE THE LOCK.
 *
 * @param host Definition of host to send this job to.
 *
 * @param status on return contains the wait-status of the remote
 * compiler.
 *
 * Returns 0 on success, otherwise error.  Returning nonzero does not
 * necessarily imply the remote compiler itself succeeded, only that
 * there were no communications problems.
 *
 * TODO: consider refactoring this (perhaps as two separate subroutines?)
 * to avoid the need for releasing the lock as a side effect of this call.
 */
int dcc_compile_remote(char **argv,
                       char *input_fname,
                       char *cpp_fname,
                       char **files,
                       char *output_fname,
                       char *deps_fname,
                       char *server_stderr_fname,
                       pid_t cpp_pid,
                       int local_cpu_lock_fd,
                       struct dcc_hostdef *host,
		       int dist_lto,
                       int *status)
{
    int to_net_fd = -1, from_net_fd = -1;
    int ret;
    pid_t ssh_pid = 0;
    int ssh_status;
    off_t doti_size;
    off_t doti_gcda_size;
    struct timeval before, after;
    unsigned int n_files;
    char *gcda_fname = NULL;
    char *gcda_tmp_fname = NULL;
    char *mangle_filename = NULL;
    char *profile_use_path = NULL;
    int profile_use_gcda = 0;
    int gcda_exist = 0;

    if (gettimeofday(&before, NULL))
        rs_log_warning("gettimeofday failed");

    dcc_note_execution(host, argv);
    dcc_note_state(DCC_PHASE_CONNECT, input_fname, host->hostname, DCC_REMOTE);

    /* For ssh support, we need to allow for separate fds writing to and
     * reading from the network, because our connection to the ssh client may
     * be over pipes, which are one-way connections. */

    *status = 0;
    if ((ret = dcc_remote_connect(host, &to_net_fd, &from_net_fd, &ssh_pid)))
        goto out;

#ifdef HAVE_GSSAPI
    /* Perform requested security. */
    if(host->authenticate) {
        rs_log_info("Performing authentication.");

        if ((ret = dcc_gssapi_perform_requested_security(host, to_net_fd, from_net_fd)) != 0) {
            rs_log_crit("Failed to perform authentication.");
            goto out;
        }

        /* Context deleted here as we no longer need it.  However, we have it available */
        /* in case we want to use confidentiality/integrity type services in the future. */
        dcc_gssapi_delete_ctx(&distcc_ctx_handle);
    } else {
        rs_log_info("No authentication requested.");
    }
#endif

    dcc_note_state(DCC_PHASE_SEND, NULL, NULL, DCC_REMOTE);

    if (host->cpp_where == DCC_CPP_ON_SERVER) {
        if ((ret = dcc_send_header(to_net_fd, argv, host))) {
          goto out;
        }

        n_files = dcc_argv_len(files);
        if ((ret = dcc_x_many_files(to_net_fd, n_files, files))) {
            goto out;
        }
    } else {
        /* This waits for cpp and puts its status in *status.  If cpp failed,
         * then the connection will have been dropped and we need not bother
         * trying to get any response from the server. */

        if ((ret = dcc_send_header(to_net_fd, argv, host)))
            goto out;

        if ((ret = dcc_wait_for_cpp(cpp_pid, status, input_fname)))
            goto out;

        /* We are done with local preprocessing.  Unlock to allow someone
         * else to start preprocessing. */
        if (local_cpu_lock_fd != -1) {
            dcc_unlock(local_cpu_lock_fd);
            local_cpu_lock_fd = -1;
        }

        if (*status != 0)
            goto out;

        if ((ret = dcc_x_file(to_net_fd, cpp_fname, "DOTI", host->compr,
                              &doti_size)))
            goto out;

	char * a;
	if (!dist_lto)
	  for (int i = 0; (a = argv[i]); i++) {
	    if (a[0] == '-') {
	      if (!strncmp(a, "-fprofile-use", 13))
		profile_use_gcda = 1;
	      if (!strncmp(a, "-fprofile-use=", 14))
		{
		  size_t len = strlen(a) - 14 + 1;
		  profile_use_path = malloc (len);
		  memset ( profile_use_path, 0, len);
		  strncpy(profile_use_path, a+14, len);
		  rs_trace("profile_use_path: %s", profile_use_path);
		}
	    }
	  }

        if (profile_use_gcda && output_fname)
	{
	   const char *tempdir;
	   if ((ret = dcc_get_tmp_top(&tempdir)))
	     return ret;

	   if (access(tempdir, W_OK|X_OK) == -1) {
	     rs_log_error("can't use TMPDIR \"%s\": %s", tempdir, strerror(errno));
	     return EXIT_IO_ERROR;
	   }

	    char cwd[PATH_MAX];
	    getcwd(cwd, sizeof(cwd));
	   const char *dot = dcc_find_extension_const(output_fname);
	   rs_trace("output_fname:%s", output_fname);
	   rs_trace("cwd:%s", cwd);
	   int dot_len = 0;
	   if (dot)
	     dot_len = strlen(dot);
	   int cwd_len = strlen(cwd);
	   int profile_use_len = 0;
	   if (profile_use_path)
	     profile_use_len = strlen(profile_use_path);
           size_t gcda_len = max(profile_use_len, cwd_len) +
                             strlen(output_fname) - dot_len +
                             strlen(".gcda") + 2;
           if (profile_use_path)
	     gcda_len += profile_use_len;
	   gcda_fname = malloc (gcda_len);
	   memset(gcda_fname, 0, gcda_len);
	   if (profile_use_path)
	     {
	       if (output_fname[0] != '/')
		 {
		   strcpy(gcda_fname, profile_use_path);
		   strcat(gcda_fname, "/");
		   mangle_filename = mangle_path (cwd);
		   strncat(gcda_fname, mangle_filename, strlen(mangle_filename));
		   strcat(gcda_fname, "#");
		 }
	       char *mangle_output = mangle_path (output_fname);
	       strncat(gcda_fname, mangle_output, strlen(mangle_output)- dot_len);
	       free(mangle_output);
	     }
	   else
	     {
	       if (output_fname[0] != '/')
		 {
		   strcpy(gcda_fname, cwd);
		   strcat(gcda_fname, "/");
		 }
	     strncat(gcda_fname, output_fname, strlen(output_fname)- dot_len);
	     }
	   strcat (gcda_fname, ".gcda");
	   rs_trace("gcda_fname:%s", gcda_fname);

	   int fd;
	   const char * dot2 = dcc_find_extension_const(cpp_fname);
	   size_t temp_len = strlen(cpp_fname) - strlen(dot2) + strlen(".gcda") + 1;
	   gcda_tmp_fname = malloc (temp_len);
	   memset(gcda_tmp_fname, 0, temp_len);

	   strncpy(gcda_tmp_fname, cpp_fname, strlen(cpp_fname) - strlen(dot2));
	   strcat (gcda_tmp_fname, ".gcda");
	   rs_trace("gcda_tmp_fname:%s", gcda_tmp_fname);

	   do {
	     int fd_src = open(gcda_fname, O_RDONLY, 0600);
	     if (fd_src == -1) {
	       rs_trace("gcda file doesn't exist %s: %s", gcda_fname, strerror(errno));
	       goto gcda_early_out;
	     }

	     fd = open(gcda_tmp_fname, O_WRONLY | O_CREAT | O_EXCL, 0600);
	     if (fd == -1) {
	       /* try again */
	       rs_trace("failed to create %s: %s", gcda_tmp_fname, strerror(errno));
	       continue;
	     }

	     char buff[1024];
	     int len;
             while ((len = read(fd_src, buff, 1024)) != 0)
	     {
	       if (len == -1)
		 goto gcda_early_out;
               write(fd, buff, len);
             }

             if (close(fd) == -1) {  /* huh? */
	       rs_log_warning("failed to close %s: %s", gcda_tmp_fname, strerror(errno));
	       goto gcda_early_out;
	     }

	     if (close(fd_src) == -1) {  /* huh? */
	       rs_log_warning("failed to close %s: %s", gcda_fname, strerror(errno));
	       goto gcda_early_out;
	     }

	     break;
	   } while (1);

	   if ((ret = dcc_add_cleanup(gcda_tmp_fname))) {
	     /* bailing out */
	     unlink(gcda_tmp_fname);
	     goto gcda_early_out;
	   }

	   gcda_exist = 1;
           if ((ret = dcc_x_token_int(to_net_fd, "GCDA", gcda_exist)) != 0) {
             rs_trace("fail to send token %s", strerror(errno));
             goto out;
           }

          if ((ret = dcc_x_file(to_net_fd, gcda_tmp_fname, "DOTI", host->compr,
		  &doti_gcda_size)))
	    goto out;
	}
    }

gcda_early_out:
    if (profile_use_gcda && !gcda_exist) {
	if ((ret = dcc_x_token_int(to_net_fd, "GCDA", 0)) != 0)
	  rs_trace("fail to send token %s", strerror(errno));
    }

    rs_trace("client finished sending request to server");
    tcp_cork_sock(to_net_fd, 0);
    /* but it might not have been read in by the server yet; there's
     * 100kB or more of buffers in the two kernels. */

    /* OK, now all of the source has at least made it into the
     * client's TCP transmission queue, sometime soon the server will
     * start compiling it.  */
    dcc_note_state(DCC_PHASE_COMPILE, NULL, host->hostname, DCC_REMOTE);

    /* If cpp failed, just abandon the connection, without trying to
     * receive results. */
    if (ret == 0 && *status == 0) {
        ret = dcc_retrieve_results(from_net_fd, status, output_fname,
                                   deps_fname, server_stderr_fname, host);
    }

    if (gettimeofday(&after, NULL)) {
        rs_log_warning("gettimeofday failed");
    } else if (host->cpp_where == DCC_CPP_ON_CLIENT) {
        double secs, rate;

        dcc_calc_rate(doti_size, &before, &after, &secs, &rate);
        rs_log(RS_LOG_INFO|RS_LOG_NONAME,
               "%lu bytes from %s compiled on %s in %.4fs, rate %.0fkB/s",
               (unsigned long) doti_size, input_fname, host->hostname,
               secs, rate);
    }

  out:
    if (local_cpu_lock_fd != -1) {
        dcc_unlock(local_cpu_lock_fd);
        local_cpu_lock_fd = -1; /* Not really needed; just for consistency. */
    }

    /* Close socket so that the server can terminate, rather than
     * making it wait until we've finished our work. */
    if (to_net_fd != from_net_fd) {
        if (to_net_fd != -1)
            dcc_close(to_net_fd);
    }
    if (from_net_fd != -1)
        dcc_close(from_net_fd);

    /* Collect the SSH child.  Strictly this is unnecessary; it might slow the
     * client down a little when things could otherwise be proceeding in the
     * background.  But it helps make sure that we don't assume we succeeded
     * when something possibly went wrong, and it allows us to account for the
     * cost of the ssh child. */
    if (ssh_pid) {
        dcc_collect_child("ssh", ssh_pid, &ssh_status, timeout_null_fd); /* ignore failure */
    }

    if (gcda_fname)
      free (gcda_fname);

    if (gcda_tmp_fname)
      free (gcda_tmp_fname);

    if (mangle_filename)
      free (mangle_filename);

    if (profile_use_path)
      free (profile_use_path);

    return ret;
}
