#!/bin/bash
# Power on the Atari (tools/power_on.sh) once main's app is up: when its
# "BOOT COMPLETE" line (main.cpp's threadFunc(), the end of setup, core 1
# has the bus) shows up in a growing console log, or blindly once a short
# timeout passes without it (as the fixed "sleep 4" before it did): a lost
# console (the USB host back late, main's output dropped) then shows up as a
# run with no boot lines, not as one that never powered on. Blind is safe
# with the Sidecar (boot.sh -sc): it holds the Atari's RESET until main
# reports BOOT COMPLETE. Run it in the background and kill it when the run
# ends (boot.sh does, on any exit), so it can't power on an Atari after its
# run is over.
#
# Usage: power_on_when_up.sh <log> <timeout_sec> [<after_pattern>]
#   <log>            the console capture being written (boot.sh's tee)
#   <timeout_sec>    how long to wait for the BOOT COMPLETE line before
#                    powering on anyway
#   <after_pattern>  only a BOOT COMPLETE line after the first line matching this ERE
#                    counts (boot.sh -sc: "is the staged app: current", so a
#                    boot that ends in an OTA reboot doesn't count)
#
# Exit codes: 0 powered on at BOOT COMPLETE, 3 powered on blind (timed out),
# 1 the plug never confirmed, 2 usage error.

if (($# < 2)) || [[ ! "$2" =~ ^[1-9][0-9]*$ ]]; then
	printf 'Usage: %s <log> <timeout_sec> [<after_pattern>]\n' "$0" >&2
	exit 2
fi
log="$1"
timeout_sec="$2"
after="${3:-}"
cd "$(dirname "$0")/.." || exit 2

up() {
	[[ -f "${log}" ]] || return 1
	if [[ -n "${after}" ]]; then
		awk -v after="${after}" '!seen && $0 ~ after { seen = 1; next } seen && /^BOOT COMPLETE/ { found = 1; exit } END { exit !found }' "${log}"
	else
		grep -aqE '^BOOT COMPLETE' "${log}"
	fi
}

deadline=$((SECONDS + timeout_sec))
while ((SECONDS < deadline)); do
	if up; then
		./tools/power_on.sh
		exit 0
	fi
	sleep 0.2
done
printf 'POWER ON BLIND: no "BOOT COMPLETE" line from main in %s s (its console output lost?); powering on anyway\n' "${timeout_sec}"
./tools/power_on.sh || exit 1
exit 3
