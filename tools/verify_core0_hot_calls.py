#!/usr/bin/env python3
"""Post-link check: hot-bus code (core 0 code that can run while core 1
serves the 6502 bus; see AGENTS.md) must stay in vetted IRAM code and must
not touch peripherals.

Core 1 drives the Atari bus through GPIO registers; a core 0 access to any
peripheral (esp_timer's SYSTIMER included) can delay those accesses and
break bus timing at once. Code that runs while core 1 is cold (e.g.
handlePbiRequest2() after the 6502 is parked) may do anything, so only the
roots below and what they call are checked. A --cold function is a
boundary: the bus is cold for its body (it detaches the bus first and
reattaches it last, as handlePbiRequest2() does), so a call to it is
accepted and not walked. A --terminal function ends the run (core0_exit(),
__assert_func): it does run with the bus hot, but only on the way out, so a
call to it is accepted the same way and reported separately. An --ignore
function is instrumentation outside the production code (the PROFILE builds'
GPIO sampler), accepted and reported the same way. From each root, every
call is followed:

  - an IRAM target must come from an allowed archive (by default main's
    and the Sidecar link's) and is walked in turn; IRAM code
    from anywhere else (esp_timer_get_time() is IRAM, and reads SYSTIMER
    through a pointer no literal reveals) fails as not vetted;
  - a ROM target must be on the allowlist (leaf routines known to touch no
    peripheral);
  - anything else (flash, unknown) fails;
  - an indirect call (callx) must resolve through the l32r literal last
    loaded into its register, or it fails;
  - an l32r literal holding a peripheral address fails (an inlined
    REG_READ/REG_WRITE).

With none of the roots linked the check is skipped.
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

# The hot-bus entry points (AGENTS.md, "Hot bus and cold bus"). Roots not
# linked into a build (the SidecarHost hook in loopback builds, everything
# Sidecar in bench builds) are just absent.
DEFAULT_ROOTS = (
    "core0Loop",
    "SidecarHost::checkPbiRequestHook",
    "SidecarHost::handlePbiRequest",
    "Sidecar::probe",
    "Sidecar::nop",
    "Sidecar::sio",
    "Sidecar::cio",
    "Sidecar::transact",
    "Sidecar::loopbackCheck",
    "i2slink_read",
    "i2slink_write",
)
DEFAULT_ARCHIVES = ("libmain.a", "libsidecar_link.a")
# Its body runs with the bus detached (ScopedInterruptEnable).
DEFAULT_COLD = ("handlePbiRequest2",)
# The run ends here: core0_exit() for every exit, __assert_func for a failed
# assert().
DEFAULT_TERMINAL = ("core0_exit", "__assert_func")
# Profiling only (PROFILE_MODE GPIO/PIN builds): samples the bus pins on
# purpose, after a one-time setup in flash.
DEFAULT_IGNORE = ("profileGpioSampleBurstForCycles",)
# ROM routines known to be leaves without peripheral access.
ROM_ALLOWED = {
    "memcpy",
    "memset",
    "memmove",
    "esp_rom_crc32_le",
    "_xtos_set_intlevel",
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
    """Sorted (start, end, archive) of the IRAM input sections in a link map."""
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
                spans.append((start, start + size, match.group(3).rsplit("/", 1)[-1]))
    spans.sort()
    return spans


def archive_of(spans, starts, address):
    index = bisect.bisect_right(starts, address) - 1
    if index >= 0 and spans[index][0] <= address < spans[index][1]:
        return spans[index][2]
    return None


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
                        help="archive whose IRAM code may be walked; repeatable (default: %s)"
                             % ", ".join(DEFAULT_ARCHIVES))
    parser.add_argument("--cold", action="append",
                        help="cold-bus boundary function: calls to it are accepted, not walked; "
                             "repeatable (default: %s)" % ", ".join(DEFAULT_COLD))
    parser.add_argument("--terminal", action="append",
                        help="terminal function (ends the run): calls to it are accepted, not "
                             "walked; repeatable (default: %s)" % ", ".join(DEFAULT_TERMINAL))
    parser.add_argument("--ignore", action="append",
                        help="instrumentation outside production code: calls to it are accepted, "
                             "not walked; repeatable (default: %s)" % ", ".join(DEFAULT_IGNORE))
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

    def addresses_of(wanted):
        return [address for name, addresses in by_name.items()
                if name == wanted or name.startswith(wanted + "(") for address in addresses]

    roots = []
    for root in args.root or DEFAULT_ROOTS:
        roots.extend(addresses_of(root))
    cold = {address for name in (args.cold or DEFAULT_COLD) for address in addresses_of(name)}
    cold_reached = set()
    terminal = {address for name in (args.terminal or DEFAULT_TERMINAL)
                for address in addresses_of(name)}
    terminal_reached = set()
    ignored = {address for name in (args.ignore or DEFAULT_IGNORE)
               for address in addresses_of(name)}
    ignored_reached = set()
    if not roots:
        print("core0 hot-call check: no root functions linked; skipped")
        return 0

    failures = []
    visited = set()
    pending = [(root, None) for root in roots]
    parent = {}      # function -> the caller it was first reached from
    rom_used = set()

    def path_to(function):
        chain = [function]
        while chain[-1] in parent and parent[chain[-1]] is not None and len(chain) < 32:
            chain.append(parent[chain[-1]])
        return " <- ".join(name_of(address) for address in chain[1:])

    while pending:
        function, caller = pending.pop()
        if function in visited:
            continue
        visited.add(function)
        parent[function] = caller
        name, size = by_address.get(function, (f"0x{function:08x}", 0))
        via = f" (via {path_to(function)})" if caller is not None else ""
        if function in cold and caller is not None:
            cold_reached.add(name)
            continue
        if function in terminal and caller is not None:
            terminal_reached.add(name)
            continue
        if function in ignored and caller is not None:
            ignored_reached.add(name)
            continue
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
                    failures.append(f"{name} loads peripheral address 0x{value:08x}: "
                                    f"{line.strip()}{via}")
                continue
            target = None
            if re.fullmatch(r"call(0|4|8|12)", mnemonic):
                target = int(operands.split()[0], 16)
            elif re.fullmatch(r"callx(0|4|8|12)", mnemonic) and first:
                target = registers.get(first.group(1))
                if target is None:
                    failures.append(f"{name} makes an unresolved indirect call: {line.strip()}"
                                    f"{via}")
                    continue
            elif first and not NO_DEST.match(mnemonic):
                registers.pop(first.group(1), None)   # overwritten
            if target is None:
                continue
            if target in terminal:
                terminal_reached.add(name_of(target))
                continue
            if in_range(target, IRAM):
                pending.append((target, function))
            elif in_range(target, ROM):
                allowed = aliases.get(target, set()) & ROM_ALLOWED
                if allowed:
                    rom_used.add(min(allowed))
                else:
                    failures.append(f"{name} calls ROM {name_of(target)} (0x{target:08x}), "
                                    f"not on the allowlist{via}")
            else:
                failures.append(f"{name} calls {name_of(target)} at 0x{target:08x}, "
                                f"outside IRAM and ROM{via}")

    if failures:
        print("core0 hot-call check FAILED: hot-bus code (reachable from the roots) must stay "
              "in vetted IRAM code and touch no peripheral", file=sys.stderr)
        counts = {}
        for failure in failures:
            counts[failure] = counts.get(failure, 0) + 1
        for failure, count in counts.items():
            print(f"  {failure}" + (f" [x{count}]" if count > 1 else ""), file=sys.stderr)
        return 1
    print(f"core0 hot-call check: {len(visited)} functions from {sorted(archives)}, "
          f"ROM {sorted(rom_used)}, cold boundaries {sorted(cold_reached)}, "
          f"terminal {sorted(terminal_reached)}"
          + (f", ignored {sorted(ignored_reached)}" if ignored_reached else "") + "; "
          f"no flash, no peripherals")
    return 0


if __name__ == "__main__":
    sys.exit(main())
