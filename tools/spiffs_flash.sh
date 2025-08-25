#!/bin/bash -ex
cd "$(dirname $0)"
cd ../main

mosquitto_pub -h 192.168.68.137 -t cmnd/tasmota_71D51D/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/tasmota_71D51D/POWER -m OFF
atr lfs/d1.atr put -l x.cmd 
~/src/arduino-esp32/tools/mkspiffs/mkspiffs -b 4096 -p 256 -s 0x420000 -c ./lfs ./spiffs.bin
esptool.py -c auto -p /dev/serial/by-id/usb-Es* write_flash 0x3D0000 ./spiffs.bin

