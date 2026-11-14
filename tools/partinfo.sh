#!/bin/bash
# Print a partition's offset or size from the project's partitions.csv, so
# scripts and Makefiles never hard-code the flash layout.
#
# Usage: partinfo.sh <name> offset|size
# Example: partinfo.sh spiffs offset   ->  0x1F0000
# PARTITIONS_CSV=<file> reads another table (the Sidecar's:
# sidecar/partitions.csv).
set -euo pipefail

name="${1:-}"
field="${2:-}"
case "$field" in
	offset) column=4 ;;
	size) column=5 ;;
	*)
		printf 'Usage: %s <name> offset|size\n' "$0" >&2
		exit 2
		;;
esac
csv="${PARTITIONS_CSV:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)/partitions.csv}"
value="$(awk -F, -v name="$name" -v column="$column" '
	{ gsub(/[ \t]/, "") }
	$1 == name { print $column; exit }' "$csv")"
if [[ -z "$value" ]]; then
	printf '%s: no partition "%s" (or no %s) in %s\n' "$0" "$name" "$field" "$csv" >&2
	exit 1
fi
printf '%s\n' "$value"
