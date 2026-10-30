#!/bin/bash
#
set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
#make -C "$SCRIPT_DIR" RUN_SEC=-1 hybrid
cd "$SCRIPT_DIR"
./make_d1.sh
ATARI800="$SCRIPT_DIR/build-atari/atari800-csim"
UNDER_GDB=0
while [[ ${1-} == --gdb ]]; do
	case "$1" in
		--gdb) UNDER_GDB=1 ;;
	esac
	shift
done

ATARI_ARGS=("$ATARI800" -xl -nopatchall ./SDX451_maxflash1.car -turbo -rambo "$@")
if (( UNDER_GDB )); then
	ATARI_ARGS=(gdb --args "${ATARI_ARGS[@]}")
fi
exec time "${ATARI_ARGS[@]}"
