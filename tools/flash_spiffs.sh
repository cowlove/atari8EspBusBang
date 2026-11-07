#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "${repo_root}"
source ./tools/export_boot_env.sh

port=${PORT:-}
usage() {
	printf 'Usage: %s [-u|--if-needed] [--noflash] [--plain-atr] [-p <serial-device>]\n' "$0" >&2
	printf '  --plain-atr  flash spiffs-plain.bin: also an uncompressed .atr beside each .atr.gz\n' >&2
	printf '               (for a board without PSRAM, e.g. a Sidecar)\n' >&2
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
		--plain-atr)
			image=spiffs-plain.bin
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
if ((if_needed)); then
	if make -q -C ./spiffs "${image}"; then
		printf './spiffs/%s is up to date; skipping build and flash.\n' "${image}"
		exit 0
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

esptool_version="$(esptool.py version | tail -n 1)"
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
esptool.py --chip auto --port "${port}" --baud 1152000 "${esptool_flash_command}" "$(./tools/partinfo.sh spiffs offset)" "./spiffs/${image}"
