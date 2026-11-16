#!/usr/bin/env python3
"""Copy files to and from the Sidecar over its USB console (USB-Serial-JTAG).

    tools/sidecar_xfer.py put <local> <remote>     e.g. put d1.atr /sd/d1.atr
    tools/sidecar_xfer.py get <remote> <local>     e.g. get /sd/d1.atr d1.atr

The port: -p, else $SIDECAR_PORT, else tools/config's SIDECAR_PORT. Nothing
else may have the port open (a cat, boot.sh -sc's capture).

Protocol (the Sidecar's console, sidecar/main/sidecarApp.cpp, "recv"/"send"):
put sends "recv <remote> <size>", waits for "XFER READY", writes the raw
bytes (USB flow-controls them), and waits for "XFER DONE <size> <sha256> <ms>";
the Sidecar writes <remote>.part and renames it when complete. get sends
"send <remote>", reads "XFER SEND <size>", then lines "@<offset><base64>
<crc32>" to "XFER DONE ..." (other console lines are the Sidecar's log,
skipped); lines that fail their CRC (now and then a stray byte from other
console output lands inside one) or never came are asked for again
("send <remote> <offset> <length>"), and the whole file is checked against
the SHA-256 from the first pass. Either way a mismatch exits 1 (for get,
nothing is written).
"""
import argparse
import base64
import hashlib
import os
import subprocess
import sys
import time
import zlib

import serial   # pyserial (in ESP-IDF's Python environment)


def default_port():
    port = os.environ.get("SIDECAR_PORT")
    if port:
        return port
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        return subprocess.run(["bash", "-c", "source tools/config >/dev/null 2>&1; echo -n $SIDECAR_PORT"],
                              cwd=root, capture_output=True, text=True, check=False).stdout or None
    except OSError:
        return None


def open_port(path):
    s = serial.Serial()
    s.port = path
    s.baudrate = 115200   # USB-Serial-JTAG ignores it
    s.timeout = 0.2
    # DTR and RTS as opening the port leaves them (both asserted): on
    # USB-Serial-JTAG, changing them (RTS without DTR) resets the chip.
    s.dtr = True
    s.rts = True
    s.open()
    s.reset_input_buffer()
    return s


def read_line(s, deadline):
    """One console line (bytes, without the newline), or None at the deadline."""
    buf = bytearray()
    while time.monotonic() < deadline:
        c = s.read(1)
        if not c:
            continue
        if c == b"\n":
            return bytes(buf).rstrip(b"\r")
        buf += c
    return None


def wait_for(s, prefixes, timeout):
    deadline = time.monotonic() + timeout
    while True:
        line = read_line(s, deadline)
        if line is None:
            sys.exit(f"sidecar_xfer: no {' or '.join(p.decode() for p in prefixes)} from the Sidecar")
        for p in prefixes:
            # The Sidecar starts these on a new line, but core 0 can still be
            # mid-line when a transfer begins: take them anywhere in a line.
            at = line.find(p)
            if at >= 0:
                return line[at:]


def check_done(line, size, sha):
    if line.startswith(b"XFER FAIL"):
        sys.exit(f"sidecar_xfer: {line.decode(errors='replace')}")
    _, _, got_size, got_sha, ms = line.decode().split()
    if int(got_size) != size or got_sha != sha:
        sys.exit(f"sidecar_xfer: MISMATCH: sent {size} bytes sha256 {sha}, "
                 f"Sidecar has {got_size} bytes sha256 {got_sha}")
    return int(ms)


def put(s, local, remote):
    data = open(local, "rb").read()
    sha = hashlib.sha256(data).hexdigest()
    s.write(f"recv {remote} {len(data)}\n".encode())
    line = wait_for(s, [b"XFER READY", b"XFER FAIL"], 10)
    if line.startswith(b"XFER FAIL"):
        sys.exit(f"sidecar_xfer: {line.decode(errors='replace')}")
    start = time.monotonic()
    for off in range(0, len(data), 4096):
        s.write(data[off:off + 4096])
    s.flush()
    ms = check_done(wait_for(s, [b"XFER DONE", b"XFER FAIL"], 30 + len(data) / 50000), len(data), sha)
    secs = time.monotonic() - start
    print(f"put {local} -> {remote}: {len(data)} bytes in {secs:.1f} s ({len(data) / secs / 1024:.0f} KB/s), "
          f"sha256 {sha} verified (Sidecar {ms} ms)")


LINE = 57   # data bytes per "@" line


def fetch(s, remote, size, chunks, rng=None):
    """One "send" pass: the lines that check out go into chunks (offset ->
    bytes). Returns the Sidecar's "XFER DONE" fields (size, sha256)."""
    cmd = f"send {remote}" + (f" {rng[0]} {rng[1]}" if rng else "")
    s.write((cmd + "\n").encode())
    line = wait_for(s, [b"XFER SEND", b"XFER FAIL"], 10)
    if line.startswith(b"XFER FAIL"):
        sys.exit(f"sidecar_xfer: {line.decode(errors='replace')}")
    length = int(line.split()[2])
    deadline = time.monotonic() + 30 + length / 20000
    while True:
        line = read_line(s, deadline)
        if line is None:
            sys.exit(f"sidecar_xfer: timed out ({cmd})")
        if line.startswith(b"@") and len(line) > 17:
            try:
                off = int(line[1:9], 16)
                data = base64.b64decode(line[9:-8], validate=True)
                crc = int(line[-8:], 16)
            except ValueError:
                continue   # garbled: asked for again below
            want = LINE if size is None else min(LINE, size - off)
            if zlib.crc32(data) == crc and off % LINE == 0 and len(data) == want:
                chunks[off] = data
        elif b"XFER DONE" in line or b"XFER FAIL" in line:
            line = line[line.find(b"XFER"):]
            if line.startswith(b"XFER FAIL"):
                sys.exit(f"sidecar_xfer: {line.decode(errors='replace')}")
            _, _, got_size, got_sha, _ = line.decode().split()
            return int(got_size), got_sha


def get(s, remote, local):
    start = time.monotonic()
    chunks = {}
    size, sha = fetch(s, remote, None, chunks)
    # The last line is short: drop a full-length guess at it (size unknown
    # during the first pass).
    for off in list(chunks):
        if len(chunks[off]) != min(LINE, size - off):
            del chunks[off]
    retried = 0
    for _ in range(5):
        missing = [off for off in range(0, size, LINE) if off not in chunks]
        if not missing:
            break
        # Ask again for each run of missing lines.
        runs = []
        for off in missing:
            if runs and runs[-1][1] == off:
                runs[-1][1] = off + LINE
            else:
                runs.append([off, off + LINE])
        for a, b in runs:
            b = min(b, size)
            fetch(s, remote, size, chunks, (a, b - a))
            retried += (b - a + LINE - 1) // LINE
    out = b"".join(chunks.get(off, b"") for off in range(0, size, LINE))
    got_sha = hashlib.sha256(out).hexdigest()
    if len(out) != size or got_sha != sha:
        sys.exit(f"sidecar_xfer: MISMATCH: Sidecar has {size} bytes sha256 {sha}, "
                 f"got {len(out)} bytes sha256 {got_sha}")
    with open(local, "wb") as f:
        f.write(out)
    secs = time.monotonic() - start
    note = f", {retried} line(s) fetched again" if retried else ""
    print(f"get {remote} -> {local}: {size} bytes in {secs:.1f} s ({size / secs / 1024:.0f} KB/s), "
          f"sha256 {sha} verified{note}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port")
    ap.add_argument("op", choices=["put", "get"])
    ap.add_argument("src")
    ap.add_argument("dst")
    args = ap.parse_args()
    port = args.port or default_port()
    if not port:
        sys.exit("sidecar_xfer: no port (-p, $SIDECAR_PORT or tools/config)")
    s = open_port(port)
    try:
        (put if args.op == "put" else get)(s, args.src, args.dst)
    finally:
        s.close()


if __name__ == "__main__":
    main()
