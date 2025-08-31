#!/bin/bash
PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_FC:01:2C:2D:90:C0-if00
echo "exit 2" > ${PORT}
