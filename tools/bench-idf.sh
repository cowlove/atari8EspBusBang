#!/bin/bash -ex 
PTEST="$1"
if [ "$PTEST" == "" ]; then PTEST=B; fi

#PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_B4\:3A\:45\:A5\:C4\:2C-if00
PORT=/dev/ttyACM0

idf.py --ccache -DPROFILEMODE=${PTEST} build flash
make -C main PORT=${PORT} cat | cat_until "DONE" |  tee out/cat.out 

touch out/timing${PTEST}.txt
mv out/timing${PTEST}.txt out/timing${PTEST}.last.txt
grep HIST out/cat.out  > out/timing${PTEST}.txt

./tools/compareTimings.sh ./out/timing${PTEST}.last.txt ./out/timing${PTEST}.txt 
