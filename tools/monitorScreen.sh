#!/bin/bash
echo -e "\033[H\033[2J" 
while sleep 1; do 
	echo -e "\033[H" 
	egrep '(DONE)' stash/*.output | sort -n | tail -`tput lines` | tail -20
	grep SCREEN stash/*.output | sort -n | tail -26 | head -26
	echo -n $(( $(date '+%s') - $(date -r start.ts '+%s') ))
	echo -n "   "

	echo -n $(( $(date '+%s') - $(date -r cat.usb-Espressif_USB_JTAG_serial_debug_unit_30\:ED\:A0\:A8\:D7\:A8-if00.out '+%s') ))
        echo "                    "
done

