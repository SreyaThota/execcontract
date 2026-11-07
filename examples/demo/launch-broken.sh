#!/bin/sh
# Defective service launcher: usage  launch-broken.sh SERVICE [ARGS...]
#
# It starts the service in whatever context its caller happened to have:
# descriptors leaked by the caller stay open, the working directory, umask
# and resource limits are inherited unchanged.
exec "$@"
