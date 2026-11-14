#!/bin/bash
# csim's FAKE_SIO stress test (main/fakeSio.h): core 0 plays the 6502's PBI
# side, writing and verifying D1: (the Sidecar's) and D2: (local) sectors to
# buffers all over memory, while atari800 runs an idle cartridge.
#   csim/a800/run_fakesio.sh            # 120 s
#   RUN_SEC=600 CSIM_SEED=7 csim/a800/run_fakesio.sh
#   BUS_CYCLE=1 csim/a800/run_fakesio.sh   # + RAMBO256 banks, $D301 bank ops
set -e
: ${RUN_SEC:=120}
: ${BUS_CYCLE:=0}
CONFIG=FAKESIO_CSIM
CART=idle.car
# Bus-cycle mode: the board serves the cartridge (FAKESIO_CSIM_BUS).
[ "$BUS_CYCLE" = 1 ] && CONFIG=FAKESIO_CSIM_BUS && CART=
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
make -C "$SCRIPT_DIR" FAKE_SIO=1 BUS_CYCLE=$BUS_CYCLE BOOT_CONFIG=$CONFIG hybrid
cd "$SCRIPT_DIR"
./make_idle_cart.py idle.car
BOOT_CONFIG=$CONFIG CSIM_RUN_SEC=$RUN_SEC exec ./build-atari/atari800-csim -xl -nopatchall $CART -turbo "$@"
