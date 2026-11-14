#!/usr/bin/env python3
"""An idle 8 KB Atari cartridge (.car) for csim's FAKE_SIO test.

The OS does its boot (PBI device init included), skips the disk boot, and
starts the cartridge, which parks the 6502 in native RAM: it stores
JMP $0600 at $0600 and jumps there. With no PBI interrupts
(BOOT_CONFIG=FAKESIO_CSIM) the 6502 then never touches the PBI device,
and core 0's fake SIO (main/fakeSio.h) has the request block to itself.

Native RAM, not the cartridge: in bus-cycle mode the board serves the
cartridge, and a fake request's bus detach (ScopedInterruptEnable,
pbi.cpp) stops it, as it stops everything the board serves: the real PBI
ROM waits in a stack-resident program for the same reason. A loop at
$A000 then ran into atari800's zeros there (BRK, then a crash).
"""
import struct
import sys

data = bytearray(8192)
park = bytes([
    0xA9, 0x4C, 0x8D, 0x00, 0x06,    # $A000: LDA #$4C, STA $0600
    0xA9, 0x00, 0x8D, 0x01, 0x06,    #        LDA #$00, STA $0601
    0xA9, 0x06, 0x8D, 0x02, 0x06,    #        LDA #$06, STA $0602
    0x4C, 0x00, 0x06,                #        JMP $0600 ($0600: JMP $0600)
])
data[0:len(park)] = park
data[0x20] = 0x60                    # $A020: RTS (the init routine)
data[0x1FFA:0x1FFC] = b"\x00\xa0"    # start address $A000
data[0x1FFC] = 0x00                  # a cartridge is present
data[0x1FFD] = 0x04                  # start it; no disk boot
data[0x1FFE:0x2000] = b"\x20\xa0"    # init address $A020
checksum = sum(data) & 0xFFFFFFFF
header = b"CART" + struct.pack(">II", 1, checksum) + b"\0\0\0\0"   # type 1: standard 8 KB
out = sys.argv[1] if len(sys.argv) > 1 else "idle.car"
with open(out, "wb") as f:
    f.write(header + data)
