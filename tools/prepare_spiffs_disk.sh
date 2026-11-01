#!/usr/bin/env bash
set -euo pipefail

disk_name=${1:?Usage: prepare_spiffs_disk.sh d1|d2|llvm_d1}
disk_gz="${disk_name}.atr.gz"
disk_raw="${disk_name}.atr"
hello_exe=.build/hello.exe
cmphalt_exe=.build/cmphalt.EXE

if ! command -v atr >/dev/null 2>&1; then
	printf 'Required command not found: atr. Install the Atari disk tools and add atr to PATH.\n' >&2
	exit 1
fi
if [[ ! -s "${hello_exe}" ]]; then
	printf 'Missing DOS test executable: %s (build it with make -C spiffs .build/hello.exe).\n' "${hello_exe}" >&2
	exit 1
fi
if [[ "${disk_name}" == d2 && ! -s "${cmphalt_exe}" ]]; then
	printf 'Missing DOS file comparator: %s (build it with make -C spiffs .build/cmphalt.EXE).\n' "${cmphalt_exe}" >&2
	exit 1
fi

stage_dir="$(mktemp -d)"
trap 'rm -rf "${stage_dir}"' EXIT
disk_image="${stage_dir}/${disk_raw}"

if [[ -f "${disk_gz}" ]]; then
	gzip -dc -- "${disk_gz}" > "${disk_image}"
elif [[ -f "${disk_raw}" ]]; then
	cp -- "${disk_raw}" "${disk_image}"
else
	atr "${disk_image}" mkfs dos2.0d
fi

case "${disk_name}" in
	d1)
		for script in x2.cmd x.cmd x256.cmd x192.cmd; do
			atr "${disk_image}" put -l "${script}"
		done
		;;
	d2)
		atr "${disk_image}" put -l x.bat
		atr "${disk_image}" put "${hello_exe}" hello.exe
		atr "${disk_image}" put "${cmphalt_exe}" cmphalt.EXE
		;;
	llvm_d1)
		atr "${disk_image}" put "${hello_exe}" autorun.sys
		;;
	*)
		printf 'Unsupported SPIFFS disk image: %s\n' "${disk_name}" >&2
		exit 2
		;;
esac

gzip -n -c "${disk_image}" > "${stage_dir}/${disk_gz}"
if ! cmp -s -- "${stage_dir}/${disk_gz}" "${disk_gz}"; then
	mv -- "${stage_dir}/${disk_gz}" "${disk_gz}"
else
	touch -- "${disk_gz}"
fi
