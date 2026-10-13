#!/bin/bash -ex
cd `dirname $0`/..
. ./tools/config
. ./tools/idf_env.sh
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
atr ./main/lfs/d1.atr put -l ./main/lfs/x2.cmd
atr ./main/lfs/d1.atr put -l ./main/lfs/x.cmd
atr ./main/lfs/d1.atr put -l ./main/lfs/x256.cmd
atr ./main/lfs/d1.atr put -l ./main/lfs/x192.cmd
atr ./main/lfs/d2.atr put -l ./main/lfs/x.bat

~/opt/llvm-mos/bin/mos-atari8-cart-std-clang ./main/lfs/simplemem.c -Oz -o ./main/lfs/simplemem.rom
~/opt/llvm-mos/bin/mos-atari8-cart-std-clang ./main/lfs/hello.c -Oz -o ./main/lfs/hello.rom
~/opt/llvm-mos/bin/mos-atari8-cart-atarimax128-clang ./main/lfs/stress.c -Oz -o ./main/lfs/stress.rom
~/opt/atari800-screen-dump/bin/cart ./main/lfs/stress.rom ./main/lfs/stress.car 41
rm ./main/lfs/stress.rom
~/opt/llvm-mos/bin/mos-atari8-dos-clang ./main/lfs/hello.c -Oz -o ./main/lfs/hello.exe
atr ./main/lfs/llvm_d1.atr put ./main/lfs/hello.exe autorun.sys
atr ./main/lfs/d2.atr put ./main/lfs/hello.exe hello.exe

rm ./main/lfs/*.elf
python "$IDF_PATH/components/spiffs/spiffsgen.py" 0x420000 ./main/lfs ./main/spiffs.bin \
	--block-size 4096 --page-size 256 --obj-name-len 32 --meta-len 4 \
	--no-magic --no-magic-len
esptool.py -c auto -p ${PORT} write_flash 0x3D0000 ./main/spiffs.bin
