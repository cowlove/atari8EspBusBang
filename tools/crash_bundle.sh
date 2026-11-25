#!/bin/bash
# After a run: if its console output shows a crash, get the core dump,
# decode it, and bundle the run's stash files, the core(s) and both apps'
# builds (main's and the Sidecar's: ELF, map, images, config, PBI ROM
# symbols) into <prefix>.crash.tgz.
#
# Usage: crash_bundle.sh [--sidecar] <port> [run prefix]
#   <port>        main's serial port; with --sidecar, the Sidecar's (boot.sh
#                 -sc, where the console is the Sidecar's)
#   [run prefix]  the stash artifact prefix, e.g.
#                 stash/20261106.152938.6cfd91ab47a2-dirty.9f48196ea34f
#                 (boot.sh passes RUN_CAPTURE_PREFIX from run_capture.sh).
#                 Default: the most recent run in stash/.
#
# What it looks for in <prefix>.output:
#   ^Backtrace:            a panic on the console's own board: its core dump
#                          partition, read with esptool on <port>, decoded
#                          against that board's ELF (main's build/, or with
#                          --sidecar the Sidecar's sidecar/build/).
#   ^COREFILE_AVAILABLE    (--sidecar) the Sidecar fetched main's core dump
#                          into its spiffs: "COREFILE_AVAILABLE main <path>
#                          <bytes> <token>", gzipped (.gz). The spiffs
#                          partition is read with esptool on <port>, the
#                          file pulled out (tools/spiffs_extract.py),
#                          gunzipped and decoded against main's ELF.
#
# Must run from the project root, with the IDF environment loaded, after the
# serial capture has released the port, and before anything else is flashed
# (the core is decoded against the current builds).
#
# Exit status: 0 no crash seen, or the bundle was written; 1 on error.
set -u

sidecar=false
if [[ "${1:-}" == --sidecar ]]; then
	sidecar=true
	shift
fi
port="${1:-}"
prefix="${2:-}"
if [[ -z "$port" ]]; then
	printf 'Usage: %s [--sidecar] <port> [run prefix]\n' "$0" >&2
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
backtrace=false
corefile=""
grep -aq '^Backtrace:' "$console" && backtrace=true
if $sidecar; then
	corefile="$(grep -a '^COREFILE_AVAILABLE main ' "$console" | tail -1 | awk '{print $3}')"
fi
if ! $backtrace && [[ -z "$corefile" ]]; then
	exit 0
fi

# Make sure no serial capture still holds the port.
pkill -f "^cat ${port}$" 2>/dev/null
sleep 0.5

status=0
# A board's own core dump partition: read it and decode it.
#   $1 the board's project dir (. or sidecar), $2 its partitions.csv,
#   $3 the core file to write, $4 the decoded info file
read_partition_core() {
	local project="$1" csv="$2" core="$3" info="$4" offset size
	offset="$(PARTITIONS_CSV="$csv" ./tools/partinfo.sh coredump offset)"
	size="$(PARTITIONS_CSV="$csv" ./tools/partinfo.sh coredump size)"
	if [[ -z "${offset:-}" || -z "${size:-}" ]]; then
		printf 'crash_bundle: no coredump partition in %s\n' "$csv" >&2
		status=1
		return
	fi
	if esptool -s --chip auto --port "$port" --baud 1152000 read-flash "$offset" "$size" "$core"; then
		decode_core "$project" "$core" "$info"
	else
		printf 'crash_bundle: reading the core dump from %s failed\n' "$port" >&2
		status=1
	fi
}
# Decode a core against a project's ELF: $1 project dir, $2 core, $3 info file.
# Main's core is checked against the ELF first: a core can come from an
# older build than build/'s (a core 1 fault never reboots main, so the next
# run's boot finds it, after a rebuild). Then the matching ELF comes from
# stash/elf (tools/elf_archive.sh, every main build), and is bundled.
decode_core() {
	local project="$1" core="$2" info="$3" elf core_sha elf_sha old
	elf="$project/build/$( [[ "$project" == . ]] && echo atari8EspBusBang || echo sidecar ).elf"
	if [[ "$project" == . && -f "$elf" ]] && core_sha="$(python3 -I ./tools/core_app_sha.py "$core")"; then
		elf_sha="$(sha256sum "$elf" | cut -d' ' -f1)"
		if [[ "$elf_sha" != "$core_sha"* ]]; then
			old="$(ls -1t stash/elf/"$core_sha"*.elf.gz 2>/dev/null | head -1)"
			if [[ -z "$old" ]]; then
				printf 'CORE_ELF_MISMATCH: the core is from app %s, %s is %.9s, and stash/elf has no %s: not decoded\n' \
					"$core_sha" "$elf" "$elf_sha" "$core_sha" | tee "$info" >&2
				return
			fi
			printf 'CORE_ELF_MISMATCH: the core is from app %s, %s is %.9s: decoding against %s (bundled as %s)\n' \
				"$core_sha" "$elf" "$elf_sha" "$old" "$prefix.elf.gz" | tee "$info" >&2
			cp -f -- "$old" "$prefix.elf.gz"
			gunzip -c -- "$old" > "$prefix.elf.tmp"
			python -m esp_coredump --chip esp32s3 info_corefile --core "$core" --core-format auto \
				"$prefix.elf.tmp" >> "$info" 2>&1 ||
				printf 'crash_bundle: esp_coredump failed; see %s\n' "$info" >&2
			rm -f -- "$prefix.elf.tmp"
			cat "$info"
			return
		fi
	fi
	if [[ -f "$elf" ]]; then
		idf.py -C "$project" coredump-info --core "$core" > "$info" 2>&1 ||
			printf 'crash_bundle: coredump-info failed; see %s\n' "$info" >&2
		cat "$info"
	else
		printf 'crash_bundle: no %s to decode against\n' "$elf" >&2
	fi
}

if $backtrace; then
	if $sidecar; then
		printf '\ncrash_bundle: "Backtrace:" in %s: the Sidecar crashed; collecting its core dump\n' "$console"
		read_partition_core sidecar sidecar/partitions.csv "$prefix.sidecar.core.bin" "$prefix.sidecar.coredump_info"
	else
		printf '\ncrash_bundle: "Backtrace:" in %s; collecting the core dump\n' "$console"
		read_partition_core . partitions.csv "$prefix.core.bin" "$prefix.coredump_info"
	fi
fi
if [[ -n "$corefile" ]]; then
	# The Sidecar's path (/spiffs/core/main.core) is the file's name inside
	# its spiffs (/core/main.core).
	name="${corefile#/spiffs}"
	printf '\ncrash_bundle: COREFILE_AVAILABLE in %s: main crashed; pulling %s out of the Sidecar'"'"'s spiffs\n' \
		"$console" "$name"
	fs="$prefix.sidecar.spiffs.bin"
	offset="$(PARTITIONS_CSV=sidecar/partitions.csv ./tools/partinfo.sh spiffs offset)"
	size="$(PARTITIONS_CSV=sidecar/partitions.csv ./tools/partinfo.sh spiffs size)"
	# The Sidecar stores it gzipped (core/main-<token>.core.gz): kept as is
	# in the bundle, and gunzipped for coredump-info.
	stored="$prefix.core.bin"
	[[ "$name" == *.gz ]] && stored="$prefix.core.bin.gz"
	if esptool -s --chip auto --port "$port" --baud 1152000 read-flash "$offset" "$size" "$fs" &&
		python3 -I ./tools/spiffs_extract.py "$fs" "$name" "$stored" &&
		{ [[ "$stored" != *.gz ]] || gunzip -c -- "$stored" > "$prefix.core.bin"; }; then
		rm -f -- "$fs"   # 7 MB, mostly disk images and main's own images; the core is out
		decode_core . "$prefix.core.bin" "$prefix.coredump_info"
	else
		printf 'crash_bundle: getting %s from the Sidecar on %s failed\n' "$name" "$port" >&2
		status=1
	fi
fi
bundle="$prefix.crash.tgz"

# Everything for this run: the stash files (console, diff, metadata, core,
# decoded info), plus the build that produced it, kept at their project paths.
dir="$(dirname "$prefix")"
base="$(basename "$prefix")"
files=()
while IFS= read -r -d '' file; do
	files+=("$(basename "$file")")
done < <(find "$dir" -maxdepth 1 -name "$base*" ! -name "$base.crash.tgz" -print0)
build_candidates=(
	# main
	build/atari8EspBusBang.elf              # app symbols: decode the core, gdb
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
	# the Sidecar app, whichever board crashed
	sidecar/build/sidecar.elf
	sidecar/build/sidecar.map
	sidecar/build/sidecar.bin
	sidecar/build/bootloader/bootloader.elf
	sidecar/build/partition_table/partition-table.bin
	sidecar/build/flasher_args.json
	sidecar/build/project_description.json
	sidecar/build/config/sdkconfig.json
	sidecar/sdkconfig
	sidecar/partitions.csv
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
