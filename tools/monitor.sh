#!/bin/bash
cd `dirname $0`
#while sleep 1; do 
	grep DONE stash/*.output | sort -n | tail -`tput lines`
	echo $(( $(date '+%s') - $(date -r cat.usb-Espressif_USB_JTAG_serial_debug_unit_30\:ED\:A0\:A8\:D7\:A8-if00.out '+%s') ))
#done
tail -1 cat.usb-Espressif_USB_JTAG_serial_debug_unit_30\:ED\:A0\:A8\:D7\:A8-if00.out
sleep 3
tail -1 cat.usb-Espressif_USB_JTAG_serial_debug_unit_30\:ED\:A0\:A8\:D7\:A8-if00.out



