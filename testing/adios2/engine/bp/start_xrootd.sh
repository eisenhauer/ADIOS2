#!/bin/sh

set -x

if [ "$(id -u)" -eq 0 ]; then
    ROOT_ARGS="-R user"
fi

"$1" -b -l /tmp/xroot.log "$ROOT_ARGS" -w "$2" -c "$2"/xroot/etc/xrootd/xrootd-ssi.cfg
