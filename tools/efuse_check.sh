#!/bin/bash
# One-time eFuse settings every main board needs, checked on each boot
# (tools/boot.sh) and burned if missing. Burns are permanent.
#
# VDD_SPI_FORCE + 3.3 V: GPIO45 is Atari D7 and also the strap that picks
# the flash supply at reset; with D7 high during an ESP reset the chip
# could pick 1.8 V and fail to boot. The eFuse makes it ignore the strap.
# Burned only on an ESP32-S3 whose PSRAM (if any) isn't a 1.8 V part:
# 1.8 V PSRAM means a 1.8 V-flash module, where 3.3 V would be wrong.
#
# UART_PRINT_CONTROL (ROM boot log off, keeping it off GPIO43 = D5) is only
# reported: CONFIG_BOOT_ROM_LOG_ALWAYS_OFF has the app burn it on its first
# boot.
#
# Never fails the boot: a read error is reported and skipped.
#
# Usage: efuse_check.sh [-p <port>]
# EFUSE_CONN overrides the espefuse connection arguments, e.g. for a dry run
# against a virtual chip:
#   EFUSE_CONN="--chip esp32s3 --virt --path-efuse-file /tmp/e.bin" tools/efuse_check.sh
set -uo pipefail
cd "$(dirname "$0")/.."
. ./tools/export_build_env.sh >/dev/null 2>&1

port="${PORT:-}"
if [[ "${1:-}" == -p ]]; then port="$2"; fi
if [[ -n "${EFUSE_CONN:-}" ]]; then
	read -r -a conn <<< "$EFUSE_CONN"
else
	conn=(--port "$port")
fi

summary="$(espefuse "${conn[@]}" summary 2>&1)"
if [[ $? -ne 0 ]] || ! grep -q "VDD_SPI_FORCE" <<< "$summary"; then
	echo "efuse_check: could not read eFuses; skipped" >&2
	tail -3 <<< "$summary" >&2
	exit 0
fi

field() { grep -E "^$1 \(BLOCK" <<< "$summary" | head -1 | sed -E 's/.*= *([^ ]+) .*/\1/'; }

if grep -q "Flash voltage (VDD_SPI) set to 3.3V by efuse" <<< "$summary"; then
	echo "efuse_check: flash voltage forced to 3.3 V: ok"
elif [[ "$(field VDD_SPI_FORCE)" == True ]]; then
	echo "efuse_check: WARNING: VDD_SPI_FORCE is burned, but not to 3.3 V:" >&2
	grep -E "Flash voltage" <<< "$summary" >&2
else
	chip="$(grep -m1 "Detecting chip type" <<< "$summary")"
	# A virtual chip (EFUSE_CONN --virt) prints no detection line.
	[[ -z "$chip" && " ${conn[*]} " == *" --chip esp32s3 "* ]] && chip="ESP32-S3 (from --chip)"
	psram="$(field PSRAM_VENDOR)"
	if [[ "$chip" != *ESP32-S3* ]]; then
		echo "efuse_check: not an ESP32-S3 ($chip); flash voltage left alone" >&2
	elif [[ "$psram" == *1v8* ]]; then
		echo "efuse_check: PSRAM $psram suggests a 1.8 V-flash module; flash voltage left alone" >&2
	else
		echo "efuse_check: burning flash voltage 3.3 V (VDD_SPI_FORCE; PSRAM vendor ${psram:-none})"
		if espefuse "${conn[@]}" --do-not-confirm set-flash-voltage 3.3V; then
			echo "efuse_check: flash voltage burned"
		else
			echo "efuse_check: WARNING: set-flash-voltage failed" >&2
		fi
	fi
fi

print="$(field UART_PRINT_CONTROL)"
if [[ "$print" == Disable ]]; then
	echo "efuse_check: ROM boot log off: ok"
else
	echo "efuse_check: ROM boot log is '$print'; the app burns it off on first boot (CONFIG_BOOT_ROM_LOG_ALWAYS_OFF)"
fi
exit 0
