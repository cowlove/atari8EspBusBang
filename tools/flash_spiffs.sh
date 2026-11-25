#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "${repo_root}"
source ./tools/export_boot_env.sh

port=${PORT:-}
usage() {
	printf 'Usage: %s [-u|--if-needed] [--noflash] [-p <serial-device>]\n' "$0" >&2
	printf 'The Sidecar has its own layout and image: sidecar/flash_spiffs.sh.\n' >&2
}

if_needed=0
no_flash=0
image=spiffs.bin
while (($#)); do
	case "$1" in
		-u|--if-needed)
			if_needed=1
			shift
			;;
		-p|--port)
			if (($# < 2)) || [[ -z "$2" ]]; then
				usage
				exit 2
			fi
			port="$2"
			shift 2
			;;
		--noflash)
			no_flash=1
			shift
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			printf 'Unknown argument: %s\n' "$1" >&2
			usage
			exit 2
			;;
	esac
done

if (( ! no_flash )) && [[ -n "${port}" ]]; then
	if [[ ! -c "${port}" ]]; then
		printf 'Serial device is not a character device: %s\n' "${port}" >&2
		exit 2
	fi

	if [[ ! -r "${port}" || ! -w "${port}" ]]; then
		printf 'Serial device needs read/write access: %s\n' "${port}" >&2
		exit 2
	fi
fi

source ./tools/export_build_env.sh
make -C ./spiffs .build/hello.exe .build/cmphalt.EXE simplemem.rom d1.atr.gz d2.atr.gz llvm_d1.atr.gz
# The board's first spiffs block against the image's: an up-to-date local
# image says nothing about the board (an erase-flash, a flash from another
# checkout). Prints why it differs; true if they match.
board_has_image() {
	local got read_cmd=read_flash
	[[ "$(esptool version | tail -n 1)" =~ ^([5-9]|[1-9][0-9]) ]] && read_cmd=read-flash
	got="$(mktemp)"
	./tools/power_off.sh >/dev/null
	if ! esptool -s --chip auto --port "${port}" --baud 1152000 --after "${FLASH_SPIFFS_AFTER:-hard-reset}" "${read_cmd}" \
			"$(./tools/partinfo.sh spiffs offset)" 4096 "${got}" >/dev/null 2>&1; then
		printf 'Could not read the board'"'"'s spiffs: flashing it.\n'
		rm -f -- "${got}"; return 1
	fi
	if cmp -s -n 4096 -- "${got}" "./spiffs/${image}"; then
		rm -f -- "${got}"; return 0
	fi
	if [[ "$(tr -d '\377' < "${got}" | wc -c)" -eq 0 ]]; then
		printf 'The board'"'"'s spiffs partition is erased: flashing it.\n'
	else
		printf 'The board'"'"'s spiffs differs from ./spiffs/%s: flashing it.\n' "${image}"
	fi
	rm -f -- "${got}"; return 1
}

if ((if_needed)); then
	if make -q -C ./spiffs "${image}"; then
		if (( no_flash )) || [[ -z "${port}" ]] || board_has_image; then
			printf './spiffs/%s is up to date (and on the board); skipping build and flash.\n' "${image}"
			exit 0
		fi
	else
		make_status=$?
		if ((make_status != 1)); then
			exit "${make_status}"
		fi
	fi
fi

make -C ./spiffs "${image}"

if (( no_flash )) || [[ -z "${port}" ]]; then
	printf 'Built ./spiffs/%s; no serial device specified, so nothing was flashed.\n' "${image}"
	exit 0
fi

esptool_version="$(esptool version | tail -n 1)"
esptool_major="${esptool_version%%.*}"
if [[ ! "${esptool_major}" =~ ^[0-9]+$ ]]; then
	printf 'Could not determine esptool version from: %s\n' "${esptool_version}" >&2
	exit 1
fi

if (( esptool_major >= 5 )); then
	esptool_flash_command=write-flash
else
	esptool_flash_command=write_flash
fi

./tools/power_off.sh
# FLASH_SPIFFS_AFTER=no-reset (boot.sh, which flashes the app next and that
# resets): no boot in between, which would run the old app on the new spiffs
# (and use up a config/next meant for this run's boot).
esptool -s --chip auto --port "${port}" --baud 1152000 --after "${FLASH_SPIFFS_AFTER:-hard-reset}" \
	"${esptool_flash_command}" "$(./tools/partinfo.sh spiffs offset)" "./spiffs/${image}"
