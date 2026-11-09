#!/bin/bash
#
set -e

: ${RUN_SEC:=300}
# BUS_CYCLE=1 (see the Makefile): the board supplies the cartridge and the
# extended memory, so atari800 gets neither, and the default boot config
# is one that has them. Otherwise CART picks atari800's cartridge, e.g.
#   CART=../../spiffs/.build/banktest.car BOOT_CONFIG=BANKTEST ./run.sh
: ${BUS_CYCLE:=0}
if [[ $BUS_CYCLE == 1 ]]; then
	: ${BOOT_CONFIG:=SDX_CSIM_BUS}
else
	: ${BOOT_CONFIG:=SDX_CSIM}
fi

export RUN_SEC
export VIDEO
export BOOT_CONFIG
export BUS_CYCLE

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
make -C "$SCRIPT_DIR" 
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

if [[ $BUS_CYCLE == 1 ]]; then
	ATARI_ARGS=("$ATARI800" -xl -nopatchall -turbo "$@")
else
	ATARI_ARGS=("$ATARI800" -xl -nopatchall "${CART:-./SDX451_maxflash1.car}" -turbo -rambo "$@")
fi
if (( UNDER_GDB )); then
	ATARI_ARGS=(gdb --args "${ATARI_ARGS[@]}")
fi
exec time "${ATARI_ARGS[@]}"
