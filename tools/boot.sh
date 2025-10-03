#!/bin/bash -ex
cd `dirname $0`/..
. ./tools/config

QUICK=false
if [ "$1" == "-q" ]; then
	shift
	QUICK=true
fi

if [ "$(ls -1tr main/lfs/* main/spiffs.bin | tail -1)" != "main/spiffs.bin" ]; then
	echo Rebuilding and flashing spiffs
	./tools/spiffs_flash.sh
fi

$QUICK || rm -rf ./build

./tools/updateGitH.sh
TAG=`date +%Y%m%d.%H%M%S`

mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF 
#(cd ~/tmp/build &&~/src/arduino-esp32/tools/esptool/esptool --chip esp32s3 -p ${PORT} -b 460800 --before=default_reset --after=hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB 0x0 bootloader/bootloader.bin 0x10000 atari8EspBusBang.bin 0x8000 partition_table/partition-table.bin 0xe000 ota_data_initial.bin)
if $QUICK; then
	idf.py ${1} --ccache app flash -p ${PORT}
else
	idf.py ${1} --ccache build flash -p ${PORT}
fi 
git diff > ./stash/${TAG}.git_diff
echo "$(git describe --abbrev=6 --dirty --always)" >> ./stash/${TAG}.git_version
( sleep 2 && mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m ON ) &
touch start.ts
(while sleep .1; do if [ -c ${PORT} ]; then stty -F ${PORT} -echo raw; cat ${PORT}; fi; done) | cat_until DONE ${PORT} | tee ./stash/${TAG}.output



