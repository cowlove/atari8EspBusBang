#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "${repo_root}"
source ./tools/export_boot_env.sh

port=${PORT}
usage() {
	printf 'Usage: %s [-u|--if-needed] [-p <serial-device>]\n' "$0" >&2
}

if_needed=0
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

if [[ -n "${port}" ]]; then
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
if ((if_needed)); then
	if make -q -C ./spiffs spiffs.bin; then
		printf './spiffs/spiffs.bin is up to date; skipping build and flash.\n'
		exit 0
	else
		make_status=$?
		if ((make_status != 1)); then
			exit "${make_status}"
		fi
	fi
fi

make -C ./spiffs

if [[ -z "${port}" ]]; then
	printf 'Built ./spiffs/spiffs.bin; no serial device specified, so nothing was flashed.\n'
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
esptool.py --chip auto --port "${port}" "${esptool_flash_command}" 0x3D0000 ./spiffs/spiffs.bin
