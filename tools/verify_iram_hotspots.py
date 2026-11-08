#!/usr/bin/env python3
import argparse
import re
import subprocess
import sys


IRAM_START = 0x40370000
IRAM_END = 0x403A0000
IRAM_BANK_SIZE = 0x8000


def parse_symbols(nm_output):
    symbols = []
    for line in nm_output.splitlines():
        match = re.match(r"^\s*([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+(\w)\s+(.+)$", line)
        if not match:
            continue
        address, size, symbol_type, name = match.groups()
        if symbol_type in "TtWw":
            symbols.append((int(address, 16), int(size, 16), name.strip()))
    return symbols


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nm", required=True)
    parser.add_argument("--elf", required=True)
    args = parser.parse_args()

    try:
        result = subprocess.run(
            [args.nm, "-S", "--size-sort", "--demangle", args.elf],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"IRAM hotspot verification could not run nm: {error}", file=sys.stderr)
        return 2

    symbols = parse_symbols(result.stdout)

    def named(prefix):
        return [symbol for symbol in symbols if symbol[2] == prefix or symbol[2].startswith(prefix + "(")]

    loop_functions = [
        symbol
        for symbol in symbols
        if "iloop_pbi<" in symbol[2]
    ]
    if len(loop_functions) != 2:
        print(f"Expected two iloop_pbi template instances, found {len(loop_functions)}", file=sys.stderr)
        return 1

    hotspot_patterns = (
        "busyWait6502Ticks",
        "bmonWaitCycles",
        "core0Loop",
        "handlePbiRequest",
    )
    optional_hotspot_patterns = (
        "profileGpioSampleForCycles",
    )
    # The Sidecar call chain: core 0 runs it from handlePbiRequest() while
    # core 1 drives the bus, polling for as long as a Sidecar request takes
    # (up to seconds). Optional: the chain is linked only with the Sidecar
    # on, and SidecarHost's hook and handler only without loopback (with it,
    # handlePbiRequest() calls Sidecar::probe() instead). Static helpers that
    # get inlined (search, copy_region, take_window) are covered by the
    # functions they are inlined into.
    sidecar_hotspot_patterns = (
        "SidecarHost::checkPbiRequestHook",
        "SidecarHost::handlePbiRequest",
        "SidecarHost::handleSio",
        "mmuCheckRangeMapped",
        "Sidecar::probe",
        "Sidecar::nop",
        "Sidecar::sio",
        "Sidecar::cio",
        "Sidecar::loopbackCheck",
        "Sidecar::transact",
        "i2slink_read",
        "i2slink_write",
        "tripwire_step",
        "t_bits_save",
        "decode_at",
        "decode_u32",
    )
    hotspots = []
    for pattern in hotspot_patterns:
        matches = named(pattern)
        if not matches:
            print(f"Expected hotspot symbol {pattern} was not found", file=sys.stderr)
            return 1
        if pattern != "busyWait6502Ticks" and len(matches) != 1:
            print(f"Expected one {pattern} symbol, found {len(matches)}", file=sys.stderr)
            return 1
        hotspots.extend(matches)

    for pattern in optional_hotspot_patterns:
        hotspots.extend(named(pattern))

    sidecar_linked = []
    for pattern in sidecar_hotspot_patterns:
        matches = named(pattern)
        if len(matches) > 1:
            print(f"Expected at most one {pattern} symbol, found {len(matches)}", file=sys.stderr)
            return 1
        hotspots.extend(matches)
        sidecar_linked.extend(name for _, _, name in matches)
    print(f"Sidecar chain: {len(sidecar_linked)} of {len(sidecar_hotspot_patterns)} functions linked")

    loop_banks = set()
    for address, size, name in loop_functions:
        end = address + size
        if size == 0 or address < IRAM_START or end > IRAM_END:
            print(f"{name} range 0x{address:08x}..0x{end:08x} is outside IRAM", file=sys.stderr)
            return 1
        first_bank = (address - IRAM_START) // IRAM_BANK_SIZE
        last_bank = (end - 1 - IRAM_START) // IRAM_BANK_SIZE
        if first_bank != last_bank:
            print(f"{name} crosses IRAM bank boundary {first_bank}->{last_bank}", file=sys.stderr)
            return 1
        loop_banks.add(first_bank)
        print(f"{name}: 0x{address:08x}..0x{end:08x}, bank {first_bank}")

    failed = False
    for address, size, name in hotspots:
        end = address + size
        if size == 0 or address < IRAM_START or end > IRAM_END:
            print(f"Hotspot {name} range 0x{address:08x}..0x{end:08x} is outside IRAM", file=sys.stderr)
            failed = True
            continue
        first_bank = (address - IRAM_START) // IRAM_BANK_SIZE
        last_bank = (end - 1 - IRAM_START) // IRAM_BANK_SIZE
        shared_banks = sorted(bank for bank in loop_banks if first_bank <= bank <= last_bank)
        if shared_banks:
            print(f"Hotspot {name} overlaps iloop_pbi IRAM bank(s) {','.join(map(str, shared_banks))}", file=sys.stderr)
            failed = True
        else:
            print(f"{name}: 0x{address:08x}..0x{end:08x}, banks {first_bank}..{last_bank}")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
