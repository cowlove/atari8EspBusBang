#!/bin/bash
tr ' ' '\n' | addr2line -f -i -e build/atari8EspBusBang.elf

