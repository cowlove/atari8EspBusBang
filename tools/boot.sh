#!/bin/bash -ex
cd `dirname $0`
cd ..
./tools/updateGitH.sh
PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_FC:01:2C:2D:90:C0-if00
TAS=tasmota_9575F5
TAG=`date +%Y%m%d.%H%M%S`

mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m OFF 
sleep 2
#(cd ~/tmp/build &&~/src/arduino-esp32/tools/esptool/esptool --chip esp32s3 -p ${PORT} -b 460800 --before=default_reset --after=hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB 0x0 bootloader/bootloader.bin 0x10000 atari8EspBusBang.bin 0x8000 partition_table/partition-table.bin 0xe000 ota_data_initial.bin)
rm -rf ./build
idf.py ${1} --ccache build flash -p ${PORT}
git diff > ./stash/${TAG}.git_diff
echo "$(git describe --abbrev=6 --dirty --always)" >> ./stash/${TAG}.git_version
( sleep 8 && mosquitto_pub -h 192.168.68.137 -t cmnd/${TAS}/POWER -m ON ) &
touch start.ts
(while sleep .1; do if [ -c ${PORT} ]; then stty -F ${PORT} -echo raw; cat ${PORT}; fi; done) | cat_until DONE | tee ./stash/${TAG}.output



