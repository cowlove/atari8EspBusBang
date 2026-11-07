#!/bin/bash
# If a run's console output shows a crash ("Backtrace:"), pull the core dump
# off the board, decode it, and bundle the run's stash files, the core and
# the matching build (ELF, map, config, PBI ROM symbols) into <prefix>.crash.tgz.
#
# Usage: crash_bundle.sh <port> [run prefix]
#   <port>        the board's serial port
#   [run prefix]  the stash artifact prefix, e.g.
#                 stash/20261106.152938.6cfd91ab47a2-dirty.9f48196ea34f
#                 (boot.sh passes RUN_CAPTURE_PREFIX from run_capture.sh).
#                 Default: the most recent run in stash/.
#
# Must run from the project root, with the IDF environment loaded, after the
# serial capture has released the port, and before anything else is flashed
# (the core is decoded against the current build/ ELF).
#
# Exit status: 0 no crash seen, or the bundle was written; 1 on error.
set -u

port="${1:-}"
prefix="${2:-}"
if [[ -z "$port" ]]; then
	printf 'Usage: %s <port> [run prefix]\n' "$0" >&2
	exit 1
fi
if [[ -z "$prefix" ]]; then
	# The newest run: every run writes a <prefix>.metadata.
	newest="$(ls -t stash/*.metadata 2>/dev/null | head -1)"
	prefix="${newest%.metadata}"
	if [[ -z "$prefix" ]]; then
		printf 'crash_bundle: no runs in stash/\n' >&2
		exit 1
	fi
fi
console="$prefix.output"
if [[ ! -f "$console" ]]; then
	printf 'crash_bundle: no console output at %s\n' "$console" >&2
	exit 1
fi
if ! grep -aq 'Backtrace:' "$console"; then
	exit 0
fi

elf="build/atari8EspBusBang.elf"
core="$prefix.core.bin"
info="$prefix.coredump_info"
bundle="$prefix.crash.tgz"
printf '\ncrash_bundle: "Backtrace:" in %s; collecting the core dump\n' "$console"

# The core dump partition, from the partition table (offset and size).
read -r core_offset core_size < <(awk -F, '$1 ~ /^ *coredump *$/ {
	gsub(/ /, "", $4); gsub(/ /, "", $5); print $4, $5 }' partitions.csv)
if [[ -z "${core_offset:-}" || -z "${core_size:-}" ]]; then
	printf 'crash_bundle: no coredump partition in partitions.csv\n' >&2
	exit 1
fi

# Make sure no serial capture still holds the port.
pkill -f "^cat ${port}$" 2>/dev/null
sleep 0.5

status=0
if esptool.py --chip auto --port "$port" --baud 1152000 \
		read_flash "$core_offset" "$core_size" "$core"; then
	if [[ -f "$elf" ]]; then
		idf.py coredump-info --core "$core" > "$info" 2>&1 ||
			printf 'crash_bundle: coredump-info failed; see %s\n' "$info" >&2
		cat "$info"
	else
		printf 'crash_bundle: no %s to decode against\n' "$elf" >&2
	fi
else
	printf 'crash_bundle: reading the core dump from %s failed\n' "$port" >&2
	status=1
fi

# Everything for this run: the stash files (console, diff, metadata, core,
# decoded info), plus the build that produced it, kept at their project paths.
dir="$(dirname "$prefix")"
base="$(basename "$prefix")"
files=()
while IFS= read -r -d '' file; do
	files+=("$(basename "$file")")
done < <(find "$dir" -maxdepth 1 -name "$base*" ! -name "$base.crash.tgz" -print0)
build_candidates=(
	"$elf"                                  # app symbols: decode the core, gdb
	build/atari8EspBusBang.map              # app link map: who owns an address
	build/atari8EspBusBang.bin              # the app image as flashed
	build/bootloader/bootloader.elf         # bootloader symbols (early-boot faults)
	build/bootloader/bootloader.map
	build/partition_table/partition-table.bin
	build/flasher_args.json                 # flash offsets and options used
	build/project_description.json          # IDF version, target, components
	build/config/sdkconfig.json             # the resolved configuration
	sdkconfig
	partitions.csv
	main/pbirom.elf                         # 6502 PBI ROM symbols (PBIROM_*)
	main/pbirom.h
	main/gitVersion.h
)
build_files=()
for file in "${build_candidates[@]}"; do
	[[ -f "$file" ]] && build_files+=("$file")
done
build_args=()
((${#build_files[@]})) && build_args=(-C "$PWD" "${build_files[@]}")
if tar -czf "$bundle" -C "$dir" "${files[@]}" "${build_args[@]}"; then
	printf 'crash_bundle: wrote %s\n' "$bundle"
else
	printf 'crash_bundle: writing %s failed\n' "$bundle" >&2
	status=1
fi
exit "$status"
