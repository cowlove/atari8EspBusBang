#!/bin/bash -ex
cd "$(dirname $0)"
cd ../main
PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_FC:01:2C:2D:90:C0-if00

mosquitto_pub -h 192.168.68.137 -t cmnd/tasmota_71D51D/POWER -m OFF
mosquitto_pub -h 192.168.68.137 -t cmnd/tasmota_71D51D/POWER -m OFF
atr lfs/d1.atr put -l lfs/x.cmd
atr lfs/d1.atr put -l lfs/x256.cmd
atr lfs/d1.atr put -l lfs/x192.cmd
atr lfs/d2.atr put -l lfs/x.bat
~/src/arduino-esp32/tools/mkspiffs/mkspiffs -b 4096 -p 256 -s 0x420000 -c ./lfs ./spiffs.bin
esptool.py -c auto -p /dev/serial/by-id/${PORT} write_flash 0x3D0000 ./spiffs.bin

