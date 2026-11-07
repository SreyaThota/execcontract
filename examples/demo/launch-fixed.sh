#!/bin/sh
# Corrected service launcher: usage  launch-fixed.sh SERVICE [ARGS...]
#
# Establishes the context the service contract (contract.json) requires.
set -eu

# POSIX sh can only name descriptors 0-9 in redirections. A production
# launcher would use close_range(2)/closefrom(3) or let systemd do it.
for fd in 3 4 5 6 7 8 9; do
    eval "exec $fd>&-"
done

cd /
umask 0027
ulimit -S -n 1024   # requires an inherited hard limit >= 1024
ulimit -S -c 0

exec "$@"
