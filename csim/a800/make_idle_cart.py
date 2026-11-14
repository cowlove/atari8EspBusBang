#!/usr/bin/env python3
"""An idle 8 KB Atari cartridge (.car) for csim's FAKE_SIO test.

The OS does its boot (PBI device init included), skips the disk boot, and
starts the cartridge, which just loops: JMP $A000. With no PBI interrupts
(BOOT_CONFIG=FAKESIO_CSIM) the 6502 then never touches the PBI device,
and core 0's fake SIO (main/fakeSio.h) has the request block to itself.
"""
import struct
import sys

data = bytearray(8192)
data[0:3] = b"\x4c\x00\xa0"          # $A000: JMP $A000
data[3] = 0x60                       # $A003: RTS (the init routine)
data[0x1FFA:0x1FFC] = b"\x00\xa0"    # start address $A000
data[0x1FFC] = 0x00                  # a cartridge is present
data[0x1FFD] = 0x04                  # start it; no disk boot
data[0x1FFE:0x2000] = b"\x03\xa0"    # init address $A003
checksum = sum(data) & 0xFFFFFFFF
header = b"CART" + struct.pack(">II", 1, checksum) + b"\0\0\0\0"   # type 1: standard 8 KB
out = sys.argv[1] if len(sys.argv) > 1 else "idle.car"
with open(out, "wb") as f:
    f.write(header + data)
