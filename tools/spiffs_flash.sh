#!/bin/bash -ex
cd `dirname $0`/..
. ./tools/config
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
atr ./main/lfs/d1.atr put -l ./main/lfs/x2.cmd
atr ./main/lfs/d1.atr put -l ./main/lfs/x.cmd
atr ./main/lfs/d1.atr put -l ./main/lfs/x256.cmd
atr ./main/lfs/d1.atr put -l ./main/lfs/x192.cmd
atr ./main/lfs/d2.atr put -l ./main/lfs/x.bat

~/opt/llvm-mos/bin/mos-atari8-cart-std-clang ./main/lfs/hello.c -Oz -o ./main/lfs/hello.rom
~/opt/llvm-mos/bin/mos-atari8-dos-clang ./main/lfs/hello.c -Oz -o ./main/lfs/hello.exe
atr ./main/lfs/llvm_d1.atr put ./main/lfs/hello.exe autorun.sys
atr ./main/lfs/d2.atr put ./main/lfs/hello.exe hello.exe

~/src/arduino-esp32/tools/mkspiffs/mkspiffs -b 4096 -p 256 -s 0x420000 -c ./main/lfs ./spiffs.bin
esptool.py -c auto -p ${PORT} write_flash 0x3D0000 ./spiffs.bin

