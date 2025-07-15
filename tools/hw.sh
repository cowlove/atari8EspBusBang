#!/bin/bash
# Build and run on hardware
PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30\:ED\:A0\:A8\:D7\:A8-if00

TAG=`date +%Y%m%d.%H%M%S`

mosquitto_pub -h 192.168.68.137 -t cmnd/tasmota_71D51D/POWER -m OFF 
git diff > stash/${TAG}.git_diff
git describe  --abbrev=8 --always > stash/${TAG}.git_commit 
make PORT=${PORT} clean
make PORT=${PORT} upload
( sleep 3 && mosquitto_pub -h 192.168.68.137 -t cmnd/tasmota_71D51D/POWER -m ON ) &
touch start.ts
make PORT=${PORT} cat | cat_until DONE
cp ./cat.`basename ${PORT}`.out stash/${TAG}.output


