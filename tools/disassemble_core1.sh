#!/bin/bash
printf "set pagination off\n disass /s iloop_pbi"  \
        | /home/jim/.espressif/tools/xtensa-esp-elf-gdb/16.2_20250324/xtensa-esp-elf-gdb/bin/xtensa-esp-elf-gdb-3.13  ./build/atari8EspBusBang.elf \
        2> /dev/null


