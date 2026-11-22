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
		--label mainapp -- "$SCRIPT_DIR/boot.sh" "$@"
fi
cd "$PROJECT_ROOT"
. ./tools/export_build_env.sh
. ./tools/export_boot_env.sh

PORT="${PORT:-${ESPPORT:-}}"
QUICK=false
QUICK_RESET=false
TESTPOOL=false
NOFLASH=false
SIDECAR_BOOT=false
ERASE_CORE=false
RUN_SEC=""
CAT_TIMEOUT=180
IDF_ARGS=()
while (($#)); do
	case "$1" in
		-qq) QUICK_RESET=true ;;
		-q) QUICK=true ;;
		-p) TESTPOOL=true ;;
		-t)
			if (($# < 2)) || [[ ! "$2" =~ ^[1-9][0-9]*$ ]]; then
				printf 'Usage: %s [-q|-qq] [-p] [-sc [--erase-core]] [-t run_seconds] [--noflash] [idf.py args...]\n' "$0" >&2
				exit 2
			fi
			RUN_SEC="$2"
			shift
			;;
		--noflash) NOFLASH=true ;;
		-sc) SIDECAR_BOOT=true ;;
		# The Sidecar built with -DERASE_CORE=1: main's core dumps are erased,
		# not fetched (bringing up a board; a core that can't be fetched).
		--erase-core) ERASE_CORE=true ;;
		*) IDF_ARGS+=("$1") ;;
	esac
	shift
done

if [[ -n "$RUN_SEC" ]]; then
	IDF_ARGS+=("-DRUN_SEC=${RUN_SEC}")
fi

# Never boot a bench-only build on a machine whose Atari this script powers
# on (MQTT_HOST and TAS set in tools/config). FAKE_CLOCK drives the 6502's
# clock, R/W and A0 from LEDC outputs, and FAKE_SIO's bank ops rewrite the
# address/data pulls and reroute R/W from core 0 (main/fakeSio.cpp): with a
# real Atari on the bus that fights its drivers and wrecks core 1's timing.
# Checks the -D args (as -DNAME=value or -D NAME=value), and for -qq, which
# boots whatever is in flash, the cached build's settings.
bench_only_define() {
	local name="$1" value
	value="${name#*=}"
	name="${name%%=*}"
	[[ "$name" == FAKE_CLOCK || "$name" == FAKE_SIO ]] || return 1
	case "${value^^}" in
		""|0|OFF|FALSE|NO|N) return 1 ;;
	esac
	return 0
}
if [[ -n "${MQTT_HOST:-}" && -n "${TAS:-}" ]]; then
	bench_only=()
	for ((i = 0; i < ${#IDF_ARGS[@]}; i++)); do
		arg="${IDF_ARGS[i]}"
		if [[ "$arg" == -D ]] && ((i + 1 < ${#IDF_ARGS[@]})); then
			arg="${IDF_ARGS[i + 1]}"
		elif [[ "$arg" == -D?* ]]; then
			arg="${arg#-D}"
		else
			continue
		fi
		if bench_only_define "$arg"; then
			bench_only+=("$arg")
		fi
	done
	if $QUICK_RESET && [[ -f build/CMakeCache.txt ]]; then
		while IFS= read -r line; do
			if bench_only_define "${line%%:*}=${line#*=}"; then
				bench_only+=("${line} (cached build)")
			fi
		done < <(grep -E '^(FAKE_CLOCK|FAKE_SIO):' build/CMakeCache.txt || true)
	fi
	if ((${#bench_only[@]})); then
		printf 'Refusing to boot: %s is a bench-only build (no Atari attached), but this machine powers on an Atari (MQTT_HOST=%s TAS=%s in tools/config).\n' \
			"${bench_only[*]}" "${MQTT_HOST}" "${TAS}" >&2
		exit 2
	fi
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
	./tools/clear_build_defines.sh build "${IDF_ARGS[@]}"
	idf.py "${IDF_ARGS[@]}" -b 1152000 --ccache app
	exit 0
fi

if $ERASE_CORE; then
	if ! $SIDECAR_BOOT || $QUICK_RESET; then
		printf -- '--erase-core needs -sc, and a Sidecar build (not -qq)\n' >&2
		exit 2
	fi
	export SIDECAR_DEFINES="${SIDECAR_DEFINES:+${SIDECAR_DEFINES} }-DERASE_CORE=1"
fi

if $SIDECAR_BOOT; then
	. ./tools/boot_sidecar.sh   # -sc: flash and boot main through the Sidecar; exits
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
	# Not rm -rf build: an earlier run's -D settings are cleared instead
	# (cache variables persist), and the build stays incremental.
	./tools/clear_build_defines.sh build "${IDF_ARGS[@]}"
	./tools/updateGitH.sh
fi

./tools/power_off.sh
# One-time eFuses (flash voltage forced to 3.3 V, so GPIO45 = D7 can't pick
# it at reset): burned here if missing, with the Atari off. Never fails.
./tools/efuse_check.sh -p "${PORT}"
# The reset-only mode boots the app already in flash; normal flashing uses idf.py.

if $QUICK_RESET; then
	esptool.py --chip auto --port "${PORT}" run
elif $QUICK; then
	idf.py "${IDF_ARGS[@]}" -b 1152000 --ccache app-flash -p "${PORT}"
else
	idf.py "${IDF_ARGS[@]}" -b 1152000 --ccache build flash -p "${PORT}"
fi
$QUICK_RESET || ./tools/elf_archive.sh   # by SHA, for decoding a core of this build later (crash_bundle.sh)
# Power on the Atari once main's app is up (its "BOOT COMPLETE" line), not after a
# fixed delay: setup's time varies (the Sidecar link, an OTA). 2 minutes at
# most. The waiter is killed on any exit, so it can't power on an Atari
# after the run is over.
boot_log="$(mktemp)"
power_pid=""
cleanup_boot() {
	if [[ -n "${power_pid}" ]]; then kill "${power_pid}" 2>/dev/null || true; fi
	rm -f -- "${boot_log}"
}
trap cleanup_boot EXIT
./tools/power_on_when_up.sh "${boot_log}" 120 &
power_pid=$!
# The serial loop must exit by itself once cat_until is done (its cat then
# dies of SIGPIPE): -e does not apply to the left side of the || below.
cat_status=0
(while sleep .1; do if [[ -c "${PORT}" ]]; then stty -F "${PORT}" -echo raw || exit; cat "${PORT}" || exit; fi; done) \
	| tee -a "${boot_log}" \
	| ./tools/cat_until.sh -t "${CAT_TIMEOUT}" "^(DONE)|(Rebooting)" "${PORT}" || cat_status=$?

# A failure exit (DONE <sec> -<n> ...) panics main after its report, for a
# core dump (main.cpp, EXIT_COREDUMP): read on to the panic's "Rebooting"
# (writing a full core takes a while), so crash_bundle.sh sees the Backtrace.
run_out="${RUN_CAPTURE_PREFIX:-}.output"
if [[ -n "${RUN_CAPTURE_PREFIX:-}" && -f "${run_out}" ]] && grep -aqE '^DONE [0-9.]+ +-[0-9]' "${run_out}"; then
	(while sleep .1; do if [[ -c "${PORT}" ]]; then stty -F "${PORT}" -echo raw || exit; cat "${PORT}" || exit; fi; done) \
		| ./tools/cat_until.sh -t 180 "Rebooting" "${PORT}" || true
fi

# A crash in the console output: pull and decode the core dump and bundle
# the run's stash files with it (<prefix>.crash.tgz).
./tools/crash_bundle.sh "${PORT}" "${RUN_CAPTURE_PREFIX:-}" || true
exit "${cat_status}"
