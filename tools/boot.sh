#!/bin/bash -e
cd `dirname $0`/..
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
	idf.py "${IDF_ARGS[@]}" --ccache fullclean
	idf.py "${IDF_ARGS[@]}" --ccache app
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
		idf.py "${IDF_ARGS[@]}" --ccache fullclean
	fi
	./tools/updateGitH.sh
fi

TAG=`date +%Y%m%d.%H%M%S`
./tools/power_off.sh
# The reset-only mode boots the app already in flash; normal flashing uses idf.py.

git diff HEAD > ./stash/${TAG}.git_diff
GIT="$(git describe --abbrev=6 --dirty --always)"
echo ${GIT} >> ./stash/${TAG}.git_version
git diff HEAD > ./stash/${TAG}.${GIT}.$(md5sum ./stash/${TAG}.git_diff | cut -c 1-6)

if $QUICK_RESET; then
	esptool.py --chip auto --port "${PORT}" run
elif $QUICK; then
	idf.py "${IDF_ARGS[@]}" --ccache fullclean
	idf.py "${IDF_ARGS[@]}" --ccache app-flash -p "${PORT}"
else
	idf.py "${IDF_ARGS[@]}" --ccache build flash -p "${PORT}"
fi 
#touch start.ts
( sleep 4 && ./tools/power_on.sh ) &
(while sleep .1; do if [[ -c "${PORT}" ]]; then stty -F "${PORT}" -echo raw; cat "${PORT}"; fi; done) \
    | ./tools/cat_until.sh -t "${CAT_TIMEOUT}" DONE "${PORT}" | tee ./stash/${TAG}.output
