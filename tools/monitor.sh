#!/bin/bash
CATF=./esp32/cat.usb-Espressif_USB_JTAG_serial_debug_unit_30\:ED\:A0\:A8\:D7\:A8-if00.out

cd `dirname $0`
#while sleep 1; do 
	grep DONE stash/*.output | sort -n | tail -`tput lines`
	echo $(( $(date '+%s') - $(date -r ${CATF} '+%s') ))
#done
tail -1 ${CATF}
sleep 3
tail -1 ${CATF}



