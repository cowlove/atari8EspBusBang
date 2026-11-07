#!/bin/bash
cd `dirname $0`/..
. ./tools/export_boot_env.sh
. ./tools/export_build_env.sh

./tools/power_off.sh
sleep 1
esptool.py -p ${PORT} -c auto read_flash "$(./tools/partinfo.sh spiffs offset)" "$(./tools/partinfo.sh spiffs size)" bin.fs
