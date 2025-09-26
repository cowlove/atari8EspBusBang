#!/bin/bash
CATF=../esp32/cat.usb-Espressif_USB_JTAG_serial_debug_unit_30\:ED\:A0\:A8\:D7\:A8-if00.out
cd `dirname $0`/..

echo -e "\033[H\033[2J" 
while sleep .01; do 
	echo -e "\033[H" 
	#egrep -a '(DONE)' ./stash/*.output | sort -n | tail -`tput lines` | tail -20
	ls -1tr ./stash/*.output | tail -1 | xargs grep SCREEN | tail -26 | head -26
        echo "                    "
	sleep 1
done

