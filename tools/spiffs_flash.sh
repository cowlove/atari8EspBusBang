#!/bin/bash -ex
cd `dirname $0`/..
. ./tools/config
cd main
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
atr lfs/d1.atr put -l lfs/x2.cmd
atr lfs/d1.atr put -l lfs/x.cmd
atr lfs/d1.atr put -l lfs/x256.cmd
atr lfs/d1.atr put -l lfs/x192.cmd
atr lfs/d2.atr put -l lfs/x.bat
~/src/arduino-esp32/tools/mkspiffs/mkspiffs -b 4096 -p 256 -s 0x420000 -c ./lfs ./spiffs.bin
esptool.py -c auto -p ${PORT} write_flash 0x3D0000 ./spiffs.bin

