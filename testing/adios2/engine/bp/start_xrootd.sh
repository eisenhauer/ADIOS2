#!/bin/sh
if [ "$(id -u)" -eq 0 ]; then
    # UID 1 means daemon in any ubuntu distro
    ROOT_ARGS="-R 1"
fi
"$1" -b -l /tmp/xroot.log "$ROOT_ARGS" -w "$2" -c "$2"/xroot/etc/xrootd/xrootd-ssi.cfg
