#!/usr/bin/env python3
"""Fail the build if core 1's hot data shares an SRAM bank with IRAM code
or core 0's interrupt stack.

The ESP32-S3's internal SRAM is one set of 32 KB banks seen two ways: IRAM
0x40370000 + n * 0x8000 and DRAM 0x3FC80000 + n * 0x8000 are bank n. IRAM
code ends, and DRAM .data begins, in the same bank. Core 1 reads its MMU
tables on every bus cycle; when cf24ba7c (system monitor off) slid them to
the front of .data, into that bank with core 0's interrupt stack and the
WiFi RX / flash IRAM code, .5 (130XE) failed within minutes. They now live
in .bss (main/mmu.h); this keeps it that way when the layout moves.

    verify_core1_hot_data.py --nm <nm> --elf <app.elf>
"""
import argparse
import subprocess
import sys

BANK_SIZE = 0x8000
IRAM_BASE = 0x40370000
DRAM_BASE = 0x3FC80000

# Core 1 hot data: what iloop_pbi() reads or writes on bus cycles.
HOT = [
    "mmuState", "l1Blocks", "dummyL1Block", "dummyL1BlockWithHalt",
    "osRomDisabledL1Block", "osRomEnabledL1Block", "osRomEnabledPbiL1Block",
    "basicEnabledL1Block", "basicDisabledL1Block",
    "d000Read", "d000Write", "pbiROM",
    "bmonArray", "bmonHead", "bmonTail", "lastPageOffset",
]
CORE0 = ["port_IntStack"]   # core 0's interrupt stack


def banks(start, size, base):
    return set(range((start - base) // BANK_SIZE, (start + max(size, 1) - 1 - base) // BANK_SIZE + 1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", required=True)
    ap.add_argument("--elf", required=True)
    args = ap.parse_args()
    syms = {}
    out = subprocess.run([args.nm, "-S", args.elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:
            syms[p[3]] = (int(p[0], 16), int(p[1], 16))
        elif len(p) == 3:
            syms.setdefault(p[2], (int(p[0], 16), 0))

    if "_iram_end" not in syms:
        sys.exit("verify_core1_hot_data: no _iram_end symbol in the ELF")
    iram_end = syms["_iram_end"][0]
    forbidden = {b: "IRAM code" for b in banks(IRAM_BASE, iram_end - IRAM_BASE, IRAM_BASE)}
    for name in CORE0:
        if name in syms:
            for b in banks(*syms[name], DRAM_BASE):
                forbidden.setdefault(b, name)

    if "mmuState" not in syms:
        sys.exit("verify_core1_hot_data: no mmuState in the ELF (renamed? update HOT)")
    present = [n for n in HOT if n in syms]   # unused ones are dropped by the linker
    bad = []
    for name in present:
        addr, size = syms[name]
        for b in sorted(banks(addr, size, DRAM_BASE)):
            if b in forbidden:
                bad.append(f"  {name} at 0x{addr:08x} (+{size}) is in SRAM bank {b}, with {forbidden[b]}")
    if bad:
        print("verify_core1_hot_data: core 1 hot data shares an SRAM bank with IRAM code or core 0's "
              "interrupt stack (keep it in .bss: main/mmu.h):")
        print("\n".join(bad))
        return 1
    hot_banks = sorted({b for n in present for b in banks(*syms[n], DRAM_BASE)})
    print(f"verify_core1_hot_data: ok: core 1 hot data in SRAM banks {hot_banks}; "
          f"IRAM code and core 0's interrupt stack in {sorted(forbidden)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
