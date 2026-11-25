#!/usr/bin/env python3
"""Real boot failures on the miner6 pool vs other machines' reboots near them.

Run on miner6:  python3 tools/boot_failure_neighbors.py [YYYYMMDD (since)]

For every capture (~/src/atari8EspBusBang.N/stash/*.mainapp.{metadata,output}):
classify the run, then for each real failure list the other machines' run
starts in its window: the whole run for runs up to 10 min, else its last
5 min (where the failure was). Baseline: the share of all classified runs,
failed or not, with a neighbor start in their first 10 min.
"""
import glob, os, re, sys
from datetime import datetime, timedelta

ROOT = os.path.expanduser("~/src")
SINCE = sys.argv[1] if len(sys.argv) > 1 else "20261101"
WIN = timedelta(minutes=10)
TAIL = timedelta(minutes=5)

def parse_ts(v):
    v = v.split(",")[0].replace("+00:00", "")
    return datetime.fromisoformat(v)

def classify(text):
    """(kind, reason): kind 'fail', 'ok' (deliberate/normal end) or 'unknown'."""
    # The Sidecar's DONE: "DONE <sec> <flag> Sidecar: ..." (since 2026-11-25), or "DONE (Sidecar: ...)"
    m = re.search(r"^DONE (?:\(|[\d.]+\s+-?\d+ )Sidecar: ([^)\n]*)", text, re.M)
    done = re.search(r"^DONE\s+([\d.]+)\s+(-?\d+)?\s*(.*)$", text, re.M)
    if done:
        reason = done.group(3).strip()
        code = done.group(2)
        if code is not None and int(code) < 0:
            return "fail", f"{code} {reason}"[:60]
        if code is None and re.search(r"^\s*-\d+ ", reason):
            return "fail", reason[:60]
        if m:
            return "fail", "Sidecar: " + m.group(1)[:50]
        if code is not None:
            return "ok", f"{code} {reason}"[:60]
        return "ok", reason[:60]
    if m:
        return "fail", "Sidecar: " + m.group(1)[:50]
    if re.search(r"MAIN_LINK_SILENT", text):
        return "fail", "MAIN_LINK_SILENT"
    if re.search(r"Guru Meditation|^Backtrace:", text, re.M):
        return "fail", "panic (no DONE)"
    return "unknown", "no DONE (killed, build failure or still running)"

runs = []
for meta in glob.glob(f"{ROOT}/atari8EspBusBang.[1-6]/stash/*.mainapp.metadata"):
    base = os.path.basename(meta)
    if base[:8] < SINCE:
        continue
    machine = meta.split("atari8EspBusBang.")[1][0]
    kv = dict(l.split("=", 1) for l in open(meta, errors="replace").read().splitlines() if "=" in l)
    if "started_utc" not in kv:
        continue
    start = parse_ts(kv["started_utc"])
    end = parse_ts(kv["finished_utc"]) if "finished_utc" in kv else None
    out = meta[: -len(".metadata")] + ".output"
    try:
        text = open(out, errors="replace").read()
    except OSError:
        continue
    kind, reason = classify(text)
    sc = " -sc" if " -sc" in kv.get("argv", "") or "-sc " in open(meta[: -len(".metadata")] + ".cmd", errors="replace").read() else ""
    runs.append(dict(m=machine, start=start, end=end, kind=kind, reason=reason, name=base[:15], sc=sc))

runs.sort(key=lambda r: r["start"])
starts = [(r["start"], r["m"], r["name"]) for r in runs]

def neighbors(r, lo, hi):
    return [(s, m, n) for s, m, n in starts if m != r["m"] and lo <= s <= hi]

def window(r):
    end = r["end"] or r["start"] + WIN
    if end - r["start"] <= WIN:
        return r["start"] - timedelta(seconds=30), end
    return end - TAIL, end

local = lambda t: (t - timedelta(hours=8)).strftime("%m-%d %H:%M:%S")   # PST
classified = [r for r in runs if r["kind"] != "unknown"]
fails = [r for r in runs if r["kind"] == "fail"]
print(f"runs since {SINCE}: {len(runs)} ({len(fails)} failures, "
      f"{sum(r['kind']=='ok' for r in runs)} ok, {sum(r['kind']=='unknown' for r in runs)} unknown)")
per = {}
for r in runs:
    d = per.setdefault(r["m"], [0, 0, 0]); d[["fail", "ok", "unknown"].index(r["kind"])] += 1
print("per machine (fail/ok/unknown): " + ", ".join(f".{m} {v[0]}/{v[1]}/{v[2]}" for m, v in sorted(per.items())))
print()
near = 0
for r in fails:
    lo, hi = window(r)
    nb = neighbors(r, lo, hi)
    near += bool(nb)
    dur = (r["end"] - r["start"]).total_seconds() / 60 if r["end"] else float("nan")
    nbs = ", ".join(f".{m} {local(s)[6:]}" for s, m, _ in nb) or "-"
    print(f".{r['m']}{r['sc']:4} {local(r['start'])} {dur:5.1f} min  {r['reason']:<45} neighbors: {nbs}")
print()
def first_win_hit(r):
    return bool(neighbors(r, r["start"] - timedelta(seconds=30), r["start"] + WIN))
base = sum(first_win_hit(r) for r in classified)
fb = sum(first_win_hit(r) for r in fails)
print(f"failures with a neighbor start in their window: {near}/{len(fails)}")
print(f"neighbor start within a run's first 10 min: failures {fb}/{len(fails)} "
      f"({100*fb/max(1,len(fails)):.0f}%), all classified runs {base}/{len(classified)} "
      f"({100*base/max(1,len(classified)):.0f}%)")
