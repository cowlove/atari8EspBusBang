#!/bin/bash
cd `dirname $0`/..
. ./tools/config
. ./tools/idf_env.sh

mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
sleep 1
esptool.py -p ${PORT} -c auto read_flash 0x3d0000 0x420000 bin.fs
