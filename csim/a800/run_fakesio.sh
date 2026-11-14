#!/bin/bash
# csim's FAKE_SIO stress test (main/fakeSio.h): core 0 plays the 6502's PBI
# side, writing and verifying D1: (the Sidecar's) and D2: (local) sectors to
# buffers all over memory, while atari800 runs an idle cartridge.
#   csim/a800/run_fakesio.sh            # 120 s
#   RUN_SEC=600 CSIM_SEED=7 csim/a800/run_fakesio.sh
set -e
: ${RUN_SEC:=120}
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
make -C "$SCRIPT_DIR" FAKE_SIO=1 BOOT_CONFIG=FAKESIO_CSIM hybrid
cd "$SCRIPT_DIR"
./make_idle_cart.py idle.car
BOOT_CONFIG=FAKESIO_CSIM CSIM_RUN_SEC=$RUN_SEC exec ./build-atari/atari800-csim -xl -nopatchall idle.car -turbo "$@"
