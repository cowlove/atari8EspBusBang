#!/usr/bin/env python3
"""Post-link check: the core 0 code that can run while core 1 drives the bus
(the Sidecar call chain) must stay in vetted IRAM code and must not touch
peripherals.

Core 1 drives the Atari bus through GPIO registers; a core 0 access to any
peripheral (esp_timer's SYSTIMER included) can delay those accesses and
break bus timing at once. Code that runs while core 1 is cold (e.g.
handlePbiRequest2() after the 6502 is parked) may do anything, so only the
roots below and what they call are checked. From each root, every call is
followed:

  - an IRAM target must come from an allowed archive (by default the
    Sidecar link's own, libsidecar_link.a) and is walked in turn; IRAM code
    from anywhere else (esp_timer_get_time() is IRAM, and reads SYSTIMER
    through a pointer no literal reveals) fails as not vetted;
  - a ROM target must be on the allowlist (leaf routines known to touch no
    peripheral);
  - anything else (flash, unknown) fails;
  - an indirect call (callx) must resolve through the l32r literal last
    loaded into its register, or it fails;
  - an l32r literal holding a peripheral address fails (an inlined
    REG_READ/REG_WRITE);
  - with --check-data, so does one holding an address in the external
    (flash or PSRAM) data window: a string or table left in flash.
--closed-object names an object file (e.g. sysMonitor.cpp.obj) whose
indirect calls are all to its own functions (its vtables and callbacks
point nowhere else): indirect calls inside it are accepted, and instead
every IRAM function of the object is checked as a root, so whichever one
an indirect call reaches has been checked. Destructors are left out: the
hot path deletes nothing.

With none of the roots linked (the Sidecar disabled) the check is skipped.
"""
import argparse
import bisect
import re
import struct
import subprocess
import sys

IRAM = (0x40370000, 0x403E0000)
ROM = (0x40000000, 0x40060000)
PERIPHERALS = (0x60000000, 0x60100000)
EXTERNAL_DATA = (0x3C000000, 0x3E000000)   # flash .rodata and PSRAM, through the cache

DEFAULT_ROOTS = (
    "Sidecar::poll",          # link v2: serving the Sidecar from core0_utilityTasks()
    "Sidecar::probe",
    "Sidecar::nop",
    "Sidecar::sio",
    "Sidecar::cio",
    "SidecarPeer::transact",
    "Sidecar::loopbackCheck",
    "i2slink_read",
    "i2slink_write",
)
DEFAULT_ARCHIVES = ("libsidecar_link.a",)
# ROM routines known to be leaves without peripheral access.
ROM_ALLOWED = {
    "memcpy",
    "memset",
    "memmove",
    "esp_rom_crc32_le",
    "_xtos_set_intlevel",
    "strlen",
}
# Instructions whose first register operand is read, not written: stores,
# branches, calls, jumps and special-register writes.
NO_DEST = re.compile(r"^(s8i|s16i|s32i|s32i\.n|s32c1i|s32e|s32nb|ssi|ssip|ssx|ssxp|"
                     r"b\w*|call\w*|j|jx|ret\w*|wsr\w*|wur\w*|xsr\w*|loop\w*|memw|"
                     r"isync|dsync|esync|rsync|extw|nop\w*)$")


def in_range(address, bounds):
    return bounds[0] <= address < bounds[1]


def load_symbols(nm, elf):
    """address -> (name, size); address -> every name; name -> addresses."""
    output = subprocess.run([nm, "-S", "-C", "--defined-only", elf], check=True,
                            capture_output=True, text=True).stdout
    by_address, aliases, by_name = {}, {}, {}
    for line in output.splitlines():
        match = re.match(r"^([0-9a-fA-F]+)\s+(?:([0-9a-fA-F]+)\s+)?(\w)\s+(.+)$", line)
        if not match:
            continue
        address = int(match.group(1), 16)
        size = int(match.group(2), 16) if match.group(2) else 0
        kind, name = match.group(3), match.group(4).strip()
        if kind not in "TtWwA":
            continue
        # Prefer a sized function symbol over an absolute alias.
        if address not in by_address or (size and not by_address[address][1]):
            by_address[address] = (name, size)
        aliases.setdefault(address, set()).add(name)
        by_name.setdefault(name, []).append(address)
    return by_address, aliases, by_name


def load_archives(map_path):
    """Sorted (start, end, archive, object) of the IRAM input sections in a link map."""
    spans = []
    placed = False
    with open(map_path, errors="replace") as handle:
        for line in handle:
            if not placed:
                placed = "Linker script and memory map" in line
                continue
            match = re.search(r"\s0x([0-9a-f]{8})\s+0x([0-9a-f]+)\s+(\S+?)\((\S+)\)\s*$", line)
            if not match:
                continue
            start, size = int(match.group(1), 16), int(match.group(2), 16)
            if size and in_range(start, IRAM):
                spans.append((start, start + size, match.group(3).rsplit("/", 1)[-1], match.group(4)))
    spans.sort()
    return spans


def span_of(spans, starts, address):
    index = bisect.bisect_right(starts, address) - 1
    if index >= 0 and spans[index][0] <= address < spans[index][1]:
        return spans[index]
    return None


def archive_of(spans, starts, address):
    span = span_of(spans, starts, address)
    return span[2] if span else None


def object_of(spans, starts, address):
    span = span_of(spans, starts, address)
    return span[3] if span else None


class Elf32:
    """Just enough ELF32 little-endian parsing to read words by address."""

    def __init__(self, path):
        self.data = open(path, "rb").read()
        (shoff,) = struct.unpack_from("<I", self.data, 0x20)
        shentsize, shnum = struct.unpack_from("<HH", self.data, 0x2E)
        self.sections = []
        for index in range(shnum):
            base = shoff + index * shentsize
            _, kind, _, address, offset, size = struct.unpack_from("<IIIIII", self.data, base)
            if address and kind != 8:   # skip SHT_NOBITS (.bss)
                self.sections.append((address, offset, size))

    def word(self, address):
        for start, offset, size in self.sections:
            if start <= address and address + 4 <= start + size:
                return struct.unpack_from("<I", self.data, offset + address - start)[0]
        return None


def disassemble(objdump, elf, start, size):
    return subprocess.run([objdump, "-d", "--no-show-raw-insn",
                           f"--start-address=0x{start:x}", f"--stop-address=0x{start + size:x}",
                           elf], check=True, capture_output=True, text=True).stdout


INSTRUCTION = re.compile(r"^\s*[0-9a-f]+:\s+(\S+)\s*(.*)$")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--nm", required=True)
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--elf", required=True)
    parser.add_argument("--map", required=True, help="the link map, for each IRAM section's archive")
    parser.add_argument("--root", action="append",
                        help="root function (demangled name, without arguments); repeatable")
    parser.add_argument("--allow-archive", action="append",
                        help="archive whose IRAM code may be walked; repeatable")
    parser.add_argument("--closed-object", action="append", default=[],
                        help="object file whose indirect calls stay inside it (see above); repeatable")
    parser.add_argument("--check-data", action="store_true",
                        help="also fail on literals that address flash or PSRAM data")
    parser.add_argument("--what", default="the Sidecar call chain",
                        help="what the roots are, for the messages")
    args = parser.parse_args()
    archives = set(args.allow_archive or DEFAULT_ARCHIVES)

    try:
        by_address, aliases, by_name = load_symbols(args.nm, args.elf)
        spans = load_archives(args.map)
        elf = Elf32(args.elf)
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"core0 hot-call check could not read the build: {error}", file=sys.stderr)
        return 2
    starts = [span[0] for span in spans]

    def name_of(address):
        return by_address.get(address, (f"0x{address:08x}", 0))[0]

    roots = []
    for root in args.root or DEFAULT_ROOTS:
        roots.extend(address for name, addresses in by_name.items()
                     if name == root or name.startswith(root + "(") for address in addresses)
    if not roots:
        print(f"core0 hot-call check ({args.what}): no root functions linked; skipped")
        return 0
    closed = set(args.closed_object)
    if closed:
        roots.extend(address for address, (name, size) in by_address.items()
                     if size and in_range(address, IRAM) and "::~" not in name
                     and object_of(spans, starts, address) in closed)

    failures = []
    visited = set()
    pending = [(root, None) for root in roots]
    rom_used = set()
    while pending:
        function, caller = pending.pop()
        if function in visited:
            continue
        visited.add(function)
        name, size = by_address.get(function, (f"0x{function:08x}", 0))
        via = f" (called from {name_of(caller)})" if caller is not None else ""
        if not in_range(function, IRAM):
            failures.append(f"{name} at 0x{function:08x} is not in IRAM{via}")
            continue
        if function not in by_address:
            failures.append(f"call to 0x{function:08x}, not a function entry (data decoded as "
                            f"code?){via}")
            continue
        archive = archive_of(spans, starts, function)
        if archive not in archives:
            failures.append(f"{name} at 0x{function:08x} is IRAM code from {archive or 'unknown'}, "
                            f"not vetted{via}")
            continue
        if not size:
            failures.append(f"{name} at 0x{function:08x} has no size; can't disassemble it{via}")
            continue
        registers = {}   # register -> value of the l32r that last loaded it
        for line in disassemble(args.objdump, args.elf, function, size).splitlines():
            match = INSTRUCTION.match(line)
            if not match:
                continue
            mnemonic, operands = match.group(1), match.group(2)
            first = re.match(r"(a\d+)\b", operands)
            if mnemonic == "l32r" and first:
                literal = re.search(r",\s*([0-9a-f]+)", operands)
                value = elf.word(int(literal.group(1), 16)) if literal else None
                registers[first.group(1)] = value
                if value is not None and in_range(value, PERIPHERALS):
                    failures.append(f"{name} loads peripheral address 0x{value:08x}: {line.strip()}")
                if args.check_data and value is not None and in_range(value, EXTERNAL_DATA):
                    failures.append(f"{name} loads flash/PSRAM data address 0x{value:08x}: {line.strip()}")
                continue
            target = None
            if re.fullmatch(r"call(0|4|8|12)", mnemonic):
                target = int(operands.split()[0], 16)
            elif re.fullmatch(r"callx(0|4|8|12)", mnemonic) and first:
                target = registers.get(first.group(1))
                if target is None:
                    if object_of(spans, starts, function) not in closed:
                        failures.append(f"{name} makes an unresolved indirect call: {line.strip()}")
                    continue
            elif first and not NO_DEST.match(mnemonic):
                registers.pop(first.group(1), None)   # overwritten
            if target is None:
                continue
            if in_range(target, IRAM):
                pending.append((target, function))
            elif in_range(target, ROM):
                allowed = aliases.get(target, set()) & ROM_ALLOWED
                if allowed:
                    rom_used.add(min(allowed))
                else:
                    failures.append(f"{name} calls ROM {name_of(target)} (0x{target:08x}), "
                                    "not on the allowlist")
            else:
                failures.append(f"{name} calls {name_of(target)} at 0x{target:08x}, "
                                "outside IRAM and ROM")

    if failures:
        print(f"core0 hot-call check FAILED: code reachable from {args.what} must stay "
              "in vetted IRAM code and touch no peripheral", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print(f"core0 hot-call check ({args.what}): {len(visited)} functions from {sorted(archives)}, "
          f"ROM {sorted(rom_used)}; no flash, no peripherals")
    return 0


if __name__ == "__main__":
    sys.exit(main())
