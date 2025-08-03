#!/bin/bash -ex 
PTEST="$1"
if [ "$PTEST" == "" ]; then PTEST=PROFB; fi


#PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_B4\:3A\:45\:A5\:C4\:2C-if00
PORT=/dev/ttyACM0
ARGS="DEF=-DTEST_SEC=30 -D${PTEST}" 

make -C main PORT=${PORT} "${ARGS}"
make -C main PORT=${PORT} "${ARGS}" upload
make -C main PORT=${PORT} "${ARGS}" cat | cat_until "DONE" |  tee timings.out 

mv timing.txt timing.last.txt
grep HIST timings.out  > timing.txt

./tools/compareTimings.sh timing.last.txt timing.txt 
