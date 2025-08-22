#!/bin/bash -ex
cd "$(dirname $0)"
cd ../main

mosquitto_pub -h 192.168.68.137 -t cmnd/tasmota_71D51D/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/tasmota_71D51D/POWER -m OFF
atr lfs/d1.atr put -l x.cmd 
littlefs_create -b 4096 -c 1056 -i ./littlefs.bin -s ./lfs/ 
esptool.py -c auto -p /dev/serial/by-id/usb-Es* write_flash 0x3D0000 ./littlefs.bin

