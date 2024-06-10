#!/bin/sh
echo "First arg is $1"
mkdir -p xroot/var/spool
mkdir -p xroot/run/xrootd
mkdir -p xroot/etc/xrootd
pwd
echo "xrootd.fslib libXrdSsi.so" > xroot/etc/xrootd/xrootd-ssi.cfg
cat xroot/etc/xrootd/xrootd-ssi.cfg
echo "all.export /home/eisen/xroot/data nolock r/w" >> xroot/etc/xrootd/xrootd-ssi.cfg
echo "oss.statlib -2 libXrdSsi.so" >> xroot/etc/xrootd/xrootd-ssi.cfg
echo "ssi.svclib $1" >> xroot/etc/xrootd/xrootd-ssi.cfg
