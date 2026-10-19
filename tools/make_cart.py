#!/usr/bin/env python3
import argparse
import struct
from pathlib import Path


HEADER_SIZE = 16
DEFAULT_CARTRIDGE_TYPE = 41
MIN_ROM_SIZE = 2 * 1024
MAX_ROM_SIZE = 128 * 1024


def main():
    parser = argparse.ArgumentParser(
        description="Wrap a raw ROM in an Atari CART image."
    )
    parser.add_argument("rom", type=Path, help="input raw ROM image")
    parser.add_argument("cart", type=Path, help="output CART image")
    parser.add_argument(
        "--type",
        type=int,
        default=DEFAULT_CARTRIDGE_TYPE,
        help="Atari CART type (40 = MegaCart 128 KiB; default 41 = AtariMax 128 KiB)",
    )
    parser.add_argument(
        "--enable-dos",
        action="store_true",
        help="Set the cartridge status bit that enables DOS",
    )
    args = parser.parse_args()

    if args.rom.resolve() == args.cart.resolve():
        parser.error("input ROM and output CART paths must differ")

    rom = args.rom.read_bytes()
    if args.type not in (40, 41):
        parser.error("supported CART types are 40 (MegaCart 128 KiB) and 41 (AtariMax 128 KiB)")
    if not MIN_ROM_SIZE <= len(rom) <= MAX_ROM_SIZE:
        parser.error("ROM size must be between 2 KiB and 128 KiB")
    if args.type == 40 and len(rom) != MAX_ROM_SIZE:
        parser.error("MegaCart type 40 requires an exactly 128 KiB ROM")
    if args.enable_dos:
        bank_size = 8 * 1024 if args.type == 41 else 16 * 1024
        dos_status_offset = bank_size - 3
        if len(rom) <= dos_status_offset:
            parser.error(
                f"ROM is too small for the type {args.type} DOS status byte at "
                f"offset {dos_status_offset}"
            )
        if rom[dos_status_offset] != 0x04:
            parser.error(
                f"unexpected type {args.type} status byte at ROM offset {dos_status_offset}: "
                f"0x{rom[dos_status_offset]:02x} (expected 0x04)"
            )
        rom = (
            rom[:dos_status_offset]
            + bytes([rom[dos_status_offset] | 0x01])
            + rom[dos_status_offset + 1:]
        )

    header = b"CART" + struct.pack(">II", args.type, 0) + bytes(4)
    args.cart.write_bytes(header + rom)

    expected_size = HEADER_SIZE + len(rom)
    actual_size = args.cart.stat().st_size
    if actual_size != expected_size:
        parser.error(f"CART size mismatch: expected {expected_size}, wrote {actual_size}")


if __name__ == "__main__":
    main()
