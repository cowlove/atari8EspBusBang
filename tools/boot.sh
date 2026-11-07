#!/bin/bash -e
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
RUN_CAPTURE_TOOL="${RUN_CAPTURE_TOOL:-$SCRIPT_DIR/run_capture.sh}"
if [[ "${RUN_CAPTURE_ACTIVE:-0}" != 1 ]]; then
	if [[ ! -x "$RUN_CAPTURE_TOOL" ]]; then
		printf 'Run capture tool is not executable: %s\n' "$RUN_CAPTURE_TOOL" >&2
		exit 2
	fi
	exec "$RUN_CAPTURE_TOOL" --repo "$PROJECT_ROOT" --output-dir "$PROJECT_ROOT/stash" \
		--label atari-boot -- "$SCRIPT_DIR/boot.sh" "$@"
fi
cd "$PROJECT_ROOT"
. ./tools/export_build_env.sh
. ./tools/export_boot_env.sh

PORT="${PORT:-${ESPPORT:-}}"
QUICK=false
QUICK_RESET=false
TESTPOOL=false
NOFLASH=false
RUN_SEC=""
CAT_TIMEOUT=200000
IDF_ARGS=()
while (($#)); do
	case "$1" in
		-qq) QUICK_RESET=true ;;
		-q) QUICK=true ;;
		-p) TESTPOOL=true ;;
		-t)
			if (($# < 2)) || [[ ! "$2" =~ ^[1-9][0-9]*$ ]]; then
				printf 'Usage: %s [-q|-qq] [-p] [-t run_seconds] [--noflash] [idf.py args...]\n' "$0" >&2
				exit 2
			fi
			RUN_SEC="$2"
			CAT_TIMEOUT=$((10#$RUN_SEC + 60))
			shift
			;;
		--noflash) NOFLASH=true ;;
		*) IDF_ARGS+=("$1") ;;
	esac
	shift
done

if [[ -n "$RUN_SEC" ]]; then
	IDF_ARGS+=("-DRUN_SEC=${RUN_SEC}")
fi

if $TESTPOOL; then
	# -p is for running test-pool hardware arrays; their numeric directory suffix becomes the TESTPOOL ID.
	HW_TYPE_SUFFIX="${PWD##*.}"
	if [[ ! "${PWD}" == *.* || ! "${HW_TYPE_SUFFIX}" =~ ^[0-9]+$ ]]; then
		printf 'Cannot determine test-pool HW_TYPE from working directory: %s (expected a numeric dot-suffix).\n' "${PWD}" >&2
		exit 2
	fi
	IDF_ARGS+=("-DHW_TYPE=\"TESTPOOL${HW_TYPE_SUFFIX}\"")
fi

#IDF_ARGS+=("-DPROFILE_MODE=OFF" "-DFAKE_CLOCK=OFF")

if $NOFLASH; then
	./tools/flash_spiffs.sh -u --noflash
	rm -rf ./build
	idf.py "${IDF_ARGS[@]}" -b 1152000 --ccache app
	exit 0
fi

if [[ -z "${PORT}" ]]; then
	printf 'PORT is unset. Set it in tools/config or export PORT=/dev/ttyACM0 before running this script.\n' >&2
	exit 2
fi
if [[ ! -c "${PORT}" ]]; then
	printf 'PORT is not a character device: %s\n' "${PORT}" >&2
	exit 2
fi

if ! $QUICK_RESET; then
	./tools/flash_spiffs.sh -u -p "${PORT}"
	if ! $QUICK; then
		rm -rf ./build
	fi
	./tools/updateGitH.sh
fi

./tools/power_off.sh
# The reset-only mode boots the app already in flash; normal flashing uses idf.py.

if $QUICK_RESET; then
	esptool.py --chip auto --port "${PORT}" run
elif $QUICK; then
	rm -rf ./build
	idf.py "${IDF_ARGS[@]}" -b 1152000 --ccache app-flash -p "${PORT}"
else
	idf.py "${IDF_ARGS[@]}" -b 1152000 --ccache build flash -p "${PORT}"
fi 
#touch start.ts
( sleep 4 && ./tools/power_on.sh ) &
# The serial loop must exit by itself once cat_until is done (its cat then
# dies of SIGPIPE): -e does not apply to the left side of the || below.
cat_status=0
(while sleep .1; do if [[ -c "${PORT}" ]]; then stty -F "${PORT}" -echo raw || exit; cat "${PORT}" || exit; fi; done) \
    | ./tools/cat_until.sh -t "${CAT_TIMEOUT}" DONE "${PORT}" || cat_status=$?

# A crash in the console output: pull and decode the core dump and bundle
# the run's stash files with it (<prefix>.crash.tgz).
./tools/crash_bundle.sh "${PORT}" "${RUN_CAPTURE_PREFIX:-}" || true
exit "${cat_status}"
