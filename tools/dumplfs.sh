#!/bin/bash
cd `dirname $0`/..
. ./tools/export_boot_env.sh
. ./tools/export_build_env.sh

./tools/power_off.sh
sleep 1
esptool.py -p ${PORT} -c auto read_flash 0x3d0000 0x420000 bin.fs
