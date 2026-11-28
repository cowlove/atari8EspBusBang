#!/bin/bash
# Power on the Atari (tools/power_on.sh) once main's app is up: when a
# growing console log shows main's "BOOT COMPLETE" line (main.cpp's
# threadFunc(), the end of setup, core 1 has the bus) or the Sidecar's
# "BOOT_EXPECTED_COMPLETE" (boot.sh -sc: 5 s after a boot report that leads
# to a normal boot, no OTA and no core fetch, with main's BOOT COMPLETE not
# yet seen; the Sidecar holds the Atari's RESET until it is). With -b, it
# powers on blindly once the timeout passes without either (plain boot.sh,
# as the fixed "sleep 4" before it did: a lost console then shows as a run
# with no boot lines); without -b it gives up (boot.sh -sc: never in the
# middle of an OTA). Run it in the background and kill it when the run ends
# (boot.sh does, on any exit), so it can't power on an Atari after its run
# is over.
#
# Usage: power_on_when_up.sh [-b] <log> <timeout_sec> [<after_pattern>]
#   -b               power on blindly at the timeout
#   <log>            the console capture being written (boot.sh's tee)
#   <timeout_sec>    how long to wait for either line
#   <after_pattern>  only a line after the first line matching this ERE
#                    counts (boot.sh -sc: "is the staged app: current", so a
#                    boot that ends in an OTA reboot doesn't count)
#
# Exit codes: 0 powered on at a ready line, 3 powered on blind (-b, timed
# out), 124 timed out (no -b), 1 the plug never confirmed, 2 usage error.

blind=false
if [[ "${1:-}" == "-b" ]]; then
	blind=true
	shift
fi
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
		awk -v after="${after}" '!seen && $0 ~ after { seen = 1; next } seen && /^(\[[0-9][0-9][0-9][0-9][0-9]\] )?(BOOT COMPLETE|BOOT_EXPECTED_COMPLETE)/ { found = 1; exit } END { exit !found }' "${log}"
	else
		grep -aqE '^(\[[0-9]{5}\] )?(BOOT COMPLETE|BOOT_EXPECTED_COMPLETE)' "${log}"
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
if ! $blind; then
	printf 'power_on_when_up: no "BOOT COMPLETE" or "BOOT_EXPECTED_COMPLETE" line in %s s; not powering on the Atari\n' "${timeout_sec}" >&2
	exit 124
fi
printf 'POWER ON BLIND: no "BOOT COMPLETE" line from main in %s s (its console output lost?); powering on anyway\n' "${timeout_sec}"
./tools/power_on.sh || exit 1
exit 3
