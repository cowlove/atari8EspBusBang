#!/usr/bin/env bash
# Build and flash the Sidecar's spiffs image (sidecar/partitions.csv: its
# own layout, not the host's). Mostly empty on purpose: room for the main
# board's core dump and for staging, plus
#   d1.atr.gz   a small compressed disk (spiffs/Makefile builds it), for
#               the bench pair's SIO tests (main's BENCH config mounts it
#               as sidecar!/d1.atr.gz); inflated into PSRAM on mount
#   main/       the main board's firmware from MAIN_BUILD (default ./build):
#               bootloader.bin, partition-table.bin, ota_data_initial.bin
#               and app.bin, for the serial flasher (console "flash") and
#               as the Firmware object main fetches (OTA)
#
# Usage: sidecar/flash_spiffs.sh [--noflash] [-p <serial-device>]
#   The port defaults to SIDECAR_PORT (tools/config). MAIN_BUILD picks the
#   main build to stage; for OTA it must use the exp-sidecar-ota layout.
set -euo pipefail

sidecar_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${sidecar_dir}/.." && pwd)"
cd -- "${repo_root}"
source ./tools/export_boot_env.sh

port=${SIDECAR_PORT:-}
no_flash=0
usage() {
	printf 'Usage: %s [--noflash] [-p <serial-device>]\n' "$0" >&2
}
while (($#)); do
	case "$1" in
		-p|--port)
			if (($# < 2)) || [[ -z "$2" ]]; then usage; exit 2; fi
			port="$2"
			shift 2
			;;
		--noflash) no_flash=1; shift ;;
		-h|--help) usage; exit 0 ;;
		*) printf 'Unknown argument: %s\n' "$1" >&2; usage; exit 2 ;;
	esac
done

source ./tools/export_build_env.sh
main_build="$(cd -- "${MAIN_BUILD:-./build}" && pwd)"
main_fw=(
	"${main_build}/bootloader/bootloader.bin"
	"${main_build}/partition_table/partition-table.bin"
	"${main_build}/ota_data_initial.bin"
	"${main_build}/atari8EspBusBang.bin"
)
for f in "${main_fw[@]}"; do
	if [[ ! -f "$f" ]]; then
		printf 'Missing main firmware %s (build main first, or set MAIN_BUILD)\n' "$f" >&2
		exit 1
	fi
done

make -C ./spiffs d1.atr.gz

export PARTITIONS_CSV="${sidecar_dir}/partitions.csv"
size="$(./tools/partinfo.sh spiffs size)"
offset="$(./tools/partinfo.sh spiffs offset)"
image="${sidecar_dir}/build/spiffs.bin"
mkdir -p -- "${sidecar_dir}/build"

stage="$(mktemp -d)"
trap 'rm -rf -- "${stage}"' EXIT
cp -- ./spiffs/d1.atr.gz "${stage}/"
mkdir -p -- "${stage}/main"
cp -- "${main_fw[0]}" "${main_fw[1]}" "${main_fw[2]}" "${stage}/main/"
cp -- "${main_fw[3]}" "${stage}/main/app.bin"
printf 'main firmware from %s -> main/\n' "${main_build}"

# The same SPIFFS format as the host's image (spiffs/Makefile): the Sidecar
# mounts it with matching settings (sidecar/sdkconfig.defaults).
python3 "${IDF_PATH}/components/spiffs/spiffsgen.py" "${size}" "${stage}" "${image}" \
	--block-size 4096 --page-size 256 --obj-name-len 32 --meta-len 4 \
	--no-magic --no-magic-len
python3 -I -c 'import sys; d = open(sys.argv[1], "rb").read(); B = 4096
free = sum(d[i:i + B] == b"\xff" * B for i in range(0, len(d), B))
print(f"{sys.argv[1]}: {free} of {len(d) // B} blocks free")' "${image}"

if ((no_flash)) || [[ -z "${port}" ]]; then
	printf 'Built %s; no serial device (SIDECAR_PORT or -p), so nothing was flashed.\n' "${image}"
	exit 0
fi
if [[ ! -c "${port}" ]]; then
	printf 'Serial device is not a character device: %s\n' "${port}" >&2
	exit 2
fi
esptool_version="$(esptool.py version | tail -n 1)"
if ((${esptool_version%%.*} >= 5)); then flash_cmd=write-flash; else flash_cmd=write_flash; fi
# The Sidecar only: no Atari power cycling (tools/power_off.sh) needed.
esptool.py --chip auto --port "${port}" --baud 1152000 "${flash_cmd}" "${offset}" "${image}"
