PORT=36002
distccd  --allow 9.134.122.0/8 --log-level warning -p $PORT --verbose --no-detach --log-stderr --wizard
netstat -anp | grep $PORT
