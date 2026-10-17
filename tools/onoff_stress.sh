#!/bin/bash -ex
cd `dirname $0`/..
. ./tools/export_boot_env.sh
. ./tools/export_build_env.sh

./tools/power_off.sh
sleep 1 
esptool.py -c auto -p ${PORT} chip_id
sleep 3
./tools/power_on.sh
sleep 5
