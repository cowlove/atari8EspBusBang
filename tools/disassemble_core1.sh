#!/bin/bash -x
cd "$(dirname $0)/.."

PYVER=3.12
if [ "$(hostname)" == "miner6" ]; then PYVER=3.13; fi
GIT="$(git describe --abbrev=6 --dirty --always)"
BRANCH="$(git branch --show-current)"


printf "set pagination off\n disass /s iloop_pbi"  \
        | /home/jim/.espressif/tools/xtensa-esp-elf-gdb/16.2_20250324/xtensa-esp-elf-gdb/bin/xtensa-esp-elf-gdb-$PYVER  ./build/atari8EspBusBang.elf \
        2> /dev/null \
	| tee ./stash/core1.${BRANCH}.${GIT}.lst

cp ./stash/core1.${BRANCH}.${GIT}.lst ./stash/core1.${BRANCH}.lst


