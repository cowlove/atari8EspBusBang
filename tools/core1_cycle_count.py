#!/usr/bin/env python3
"""Static cycle count of one core 1 bus cycle (iloop_pbi), per sample point.

Counts from the first instruction after the branch that leaves the
clock-fall poll loop (cycle 0 = that instruction issuing) to each sample
point, on the read path (R/W branch falls through) and the write path
(taken). Model: 1 cycle per instruction, taken branch or forward jump 3,
not-taken branch 1, `loopnez n` = n * (body length) + 2 (measured on
hardware for both the one- and two-nop loopNezDelay()). Every point after
the GPIO_IN address read is shown "+L": it also waits for that APB read.

  P1 address read  l32i from GPIO_IN_REG     (0x6000403c)
  P2 pin enable    s32i to GPIO_ENABLE1_W1TS (0x60004030)
  P3 read data     s32i to GPIO_OUT1_REG     (0x60004010)   read path
  P4 read release  s32i to GPIO_ENABLE1_W1TC (0x60004034)   read path
  P5 write sample  l32i from GPIO_IN1_REG    (0x60004040)   write path
  P6 write release s32i to GPIO_ENABLE1_W1TC (0x60004034)   write path

The counts are relative: compare builds with them (e.g. tail points not
earlier than master's), then measure a build with PROFILE_MODE=PIN on the
bench for real times. It can't see fetch stalls, store-buffer effects or
APB stalls beyond L.

Run as a post-build step; it reports and never fails the build.
"""
import argparse
import re
import subprocess
import sys

GPIO = {0x6000403c: "IN", 0x60004030: "W1TS", 0x60004010: "OUT1",
        0x60004034: "W1TC", 0x60004040: "IN1"}
INSTANCES = (("D500_ADDR", "_Z9iloop_pbiIL14CartBankSelect0EEvv"),
             ("D500_DATA", "_Z9iloop_pbiIL14CartBankSelect1EEvv"))


class CountError(Exception):
    pass


def target_of(ops):
    m = re.search(r"\b([0-9a-f]{8})\b", ops.split(",")[-1])
    if not m:
        raise CountError(f"no branch target in '{ops}'")
    return int(m.group(1), 16)


def gpio_of(ops):
    lit = re.search(r"\((6000[0-9a-f]{4})", ops)
    return GPIO.get(int(lit.group(1), 16)) if lit else None


def track(regmap, mn, ops):
    """Update the register -> GPIO register map for one instruction."""
    regs = [o.strip() for o in ops.split(",")]
    if not regs or not regs[0].startswith("a"):
        return
    if mn == "l32r":
        g = gpio_of(ops)
        if g:
            regmap[regs[0]] = g
        else:
            regmap.pop(regs[0], None)
    elif not re.match(r"s(32|16|8)", mn) and not mn.startswith("b") \
            and mn not in ("j", "loopnez", "nop", "nop.n"):
        regmap.pop(regs[0], None)   # anything else that writes regs[0]


def disassemble(nm, objdump, elf, sym):
    out = subprocess.run([nm, "-S", elf], capture_output=True, text=True,
                         check=True).stdout
    m = re.search(r"^([0-9a-f]+) ([0-9a-f]+) \w " + sym + "$", out, re.M)
    if not m:
        raise CountError(f"{sym} not found")
    start, size = int(m.group(1), 16), int(m.group(2), 16)
    dis = subprocess.run([objdump, "-d", f"--start-address={start:#x}",
                          f"--stop-address={start + size:#x}", elf],
                         capture_output=True, text=True, check=True).stdout
    insns = []
    for line in dis.splitlines():
        mm = re.match(r"^\s*([0-9a-f]+):\s+[0-9a-f]+\s+(\S+)\s*(.*)$", line)
        if mm:
            insns.append((int(mm.group(1), 16), mm.group(2), mm.group(3)))
    return start, insns


def count(insns):
    index = {a: i for i, (a, _, _) in enumerate(insns)}
    # entry (the cycle's head): the most common target of the clock poll's
    # beqz exits (an ee.get_gpio_in, then a beqz to the head), excluding the
    # startup sync loops that branch to themselves.
    targets = {}
    for k in range(1, len(insns)):
        a, mn, ops = insns[k]
        if mn.startswith("beqz") and insns[k - 1][1] == "ee.get_gpio_in":
            t = target_of(ops)
            if t != insns[k - 1][0]:
                targets[t] = targets.get(t, 0) + 1
    if not targets:
        raise CountError("clock poll exit branch not found")
    entry = index.get(max(targets, key=targets.get))
    if entry is None:
        raise CountError("clock poll exit target not an instruction")
    prologue = {}
    for a, mn, ops in insns[:entry]:
        track(prologue, mn, ops)

    results = {}
    for path in ("rd", "wr"):
        cyc, i, movi, after_in = 0, entry, {}, False
        reg_gpio = dict(prologue)
        points = {}
        rw_branch_seen = False
        while True:
            if i >= len(insns):
                raise CountError(f"{path} path ran off the function")
            a, mn, ops = insns[i]
            regs = [o.strip() for o in ops.split(",")]
            if mn in ("movi", "movi.n"):
                try:
                    movi[regs[0]] = int(regs[1], 0)
                except ValueError:
                    movi.pop(regs[0], None)
            if mn.startswith("l32i") and reg_gpio.get(regs[1]) in ("IN", "IN1"):
                name = "P1" if reg_gpio[regs[1]] == "IN" else "P5"
                points.setdefault(name, (cyc, after_in))
                after_in = True
            if mn.startswith("s32i") and regs[1] in reg_gpio:
                g = reg_gpio[regs[1]]
                name = {"W1TS": "P2", "OUT1": "P3",
                        "W1TC": "P4" if path == "rd" else "P6"}.get(g)
                if name:
                    points.setdefault(name, (cyc, after_in))
            track(reg_gpio, mn, ops)
            if mn == "j":
                t = target_of(ops)
                if t <= a:
                    break           # back to the clock poll: cycle done
                cyc += 3            # forward jump (old loopNezDelay's j over .align)
                i = index[t]
                continue
            if mn == "loopnez":
                end = index[target_of(ops)]
                if regs[0] not in movi:
                    raise CountError(f"loop count in {regs[0]} unknown at {a:#x}")
                cyc += movi[regs[0]] * (end - (i + 1)) + 2
                i = end
                continue
            if mn == "ee.get_gpio_in" and i != entry:
                break               # the end-of-cycle clock check: cycle done
            if mn.startswith("b"):
                if rw_branch_seen:
                    raise CountError(f"unexpected second branch {mn} at {a:#x}")
                rw_branch_seen = True
                # bbci on the R/W bit is taken for writes, bbsi for reads.
                taken_is_wr = not mn.startswith("bbs")
                if (path == "wr") == taken_is_wr:
                    cyc += 3
                    i = index[target_of(ops)]
                else:
                    cyc += 1
                    i += 1
                continue
            cyc += 1
            i += 1
        results[path] = points
    return results


def fmt(points):
    return "  ".join(f"{k} {c}{'+L' if late else ''}"
                     for k, (c, late) in sorted(points.items()))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--nm", required=True)
    ap.add_argument("--objdump", required=True)
    ap.add_argument("--elf", required=True)
    args = ap.parse_args()
    for label, sym in INSTANCES:
        try:
            start, insns = disassemble(args.nm, args.objdump, args.elf, sym)
            res = count(insns)
            print(f"CORE1 CYCLES {label} @{start:#x}: "
                  f"rd {fmt(res['rd'])} | wr {fmt(res['wr'])}")
        except (CountError, subprocess.CalledProcessError, OSError) as e:
            print(f"CORE1 CYCLES {label}: not counted ({e})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
