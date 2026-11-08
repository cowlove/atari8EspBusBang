#!/bin/bash
# dump_iloop_pbi.sh ELF [OUT]: disassemble every iloop_pbi<> instantiation
# (core 1's bus loop) to OUT (default stdout), for comparing builds: any
# change that is meant to leave the loop alone (e.g. the iloop_pbi_cycle()
# split, or csim plumbing) must give an identical dump, addresses included.
#   tools/dump_iloop_pbi.sh build/atari8EspBusBang.elf before.lst
#   ... change, rebuild ...
#   tools/dump_iloop_pbi.sh build/atari8EspBusBang.elf after.lst
#   cmp before.lst after.lst
set -euo pipefail
elf=$1
out=${2:-/dev/stdout}
NM=${NM:-xtensa-esp32s3-elf-nm}
OBJDUMP=${OBJDUMP:-xtensa-esp32s3-elf-objdump}
{
"$NM" -SC "$elf" | grep " iloop_pbi<" | while read -r addr size _ name; do
    echo "== $name size 0x$size"
    "$OBJDUMP" -d --no-show-raw-insn --start-address=0x$addr \
        --stop-address=$(printf 0x%x $((0x$addr + 0x$size))) "$elf" | grep -E "^\s*[0-9a-f]+:"
done
} > "$out"
