#!/bin/bash
# The shutdown screenshot end to end on csim: bus-cycle mode, the board
# serving make_modes_cart.py's cartridge (a band of each ANTIC mode, all in
# its ROM; boot config MODES_CSIM_BUS); main's shutdown report dumps the
# screen (main/screenshot.cpp) and tools/screenshot_png.py renders it.
#   csim/a800/run_modes.sh        # -> build-atari/modes.log, build-atari/modes.png
set -e
: ${RUN_SEC:=15}
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
make -C "$SCRIPT_DIR" BUS_CYCLE=1 BOOT_CONFIG=MODES_CSIM_BUS hybrid
cd "$SCRIPT_DIR"
./make_modes_cart.py modes.car
BOOT_CONFIG=MODES_CSIM_BUS CSIM_RUN_SEC=$RUN_SEC ./build-atari/atari800-csim -xl -nopatchall -turbo "$@" \
    > build-atari/modes.log 2>&1
grep -a '^DONE' build-atari/modes.log
../../tools/screenshot_png.py build-atari/modes.log build-atari/modes.png
