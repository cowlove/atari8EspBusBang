#!/usr/bin/env python3
import argparse
import struct
from pathlib import Path


HEADER_SIZE = 16
CARTRIDGE_TYPE = 41
MIN_ROM_SIZE = 2 * 1024
MAX_ROM_SIZE = 128 * 1024


def main():
    parser = argparse.ArgumentParser(
        description="Wrap a raw ROM as an AtariMax 128 KB CART image."
    )
    parser.add_argument("rom", type=Path, help="input raw ROM image")
    parser.add_argument("cart", type=Path, help="output CART image")
    args = parser.parse_args()

    if args.rom.resolve() == args.cart.resolve():
        parser.error("input ROM and output CART paths must differ")

    rom = args.rom.read_bytes()
    if not MIN_ROM_SIZE <= len(rom) <= MAX_ROM_SIZE:
        parser.error("Type 41 ROM size must be between 2 KiB and 128 KiB")

    header = b"CART" + struct.pack(">II", CARTRIDGE_TYPE, 0) + bytes(4)
    args.cart.write_bytes(header + rom)

    expected_size = HEADER_SIZE + len(rom)
    actual_size = args.cart.stat().st_size
    if actual_size != expected_size:
        parser.error(f"CART size mismatch: expected {expected_size}, wrote {actual_size}")


if __name__ == "__main__":
    main()
