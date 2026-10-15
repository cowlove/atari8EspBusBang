#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "${repo_root}"

usage() {
	printf 'Usage: %s [-p <serial-device>]\n' "$0" >&2
}

port=""
while (($#)); do
	case "$1" in
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

source ./tools/idf_env.sh
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

esptool.py --chip auto --port "${port}" "${esptool_flash_command}" 0x3D0000 ./spiffs/spiffs.bin
