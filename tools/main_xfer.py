#!/usr/bin/env python3
"""Copy files to and from main's spiffs, through the Sidecar.

    tools/main_xfer.py put <local> <main path>     e.g. put d1.atr.gz /spiffs/d1.atr.gz
    tools/main_xfer.py get <main path> <local>     e.g. get /spiffs/d1.atr.gz d1.atr.gz

Two hops: tools/sidecar_xfer.py's transfer to or from a staging file on
the Sidecar (--stage, default /sd/<name>.mainxfer), and the Sidecar
console's "cp" to or from "main:<path>" over the link. Then the Sidecar's
"hash main:<path>" is checked against the local file's SHA-256, and the
staging file removed (--keep leaves it).

Main serves its files only in diagnostic mode: on its console, "diag" then
"reset" (the run's next boot.sh -sc puts it back). The port: -p, else
$SIDECAR_PORT, else tools/config's; nothing else may hold it open.
"""
import argparse
import hashlib
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sidecar_xfer   # noqa: E402


def command(s, line, ends, timeout):
    """Send a console line; return the first line containing one of ends."""
    s.write(f"{line}\n".encode())
    deadline = time.monotonic() + timeout
    while True:
        got = sidecar_xfer.read_line(s, deadline)
        if got is None:
            sys.exit(f"main_xfer: no answer to '{line}' from the Sidecar")
        text = got.decode(errors="replace")
        for e in ends:
            if e in text:
                # Keep from the "Sidecar:" before it (core 0 may have been mid-line).
                at = text.rfind("Sidecar:", 0, text.find(e) + 1)
                return text[at if at >= 0 else text.find(e):]


def sidecar_cp(s, src, dst, size):
    # Terminal lines: the result ("cp <src> <dst>: ... bytes" or "FAILED"),
    # and the errors that return without one.
    line = command(s, f"cp {src} {dst}",
                   [f"Sidecar: cp {src} {dst}: ", "isn't in diagnostic mode", "open failed", "Sidecar: cp: can't"],
                   30 + size / 15000)
    print(line)
    if f"Sidecar: cp {src} {dst}: " not in line or line.endswith("FAILED"):
        sys.exit(1)


def main_hash(s, path):
    line = command(s, f"hash main:{path}", [f"Sidecar: hash main:{path}: "], 90)
    fields = line.split(": ", 2)[2].split()
    if len(fields) < 2 or len(fields[0]) != 64:
        sys.exit(f"main_xfer: {line}")
    return fields[0], int(fields[1])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port")
    ap.add_argument("--stage", help="the staging file on the Sidecar (default /sd/<name>.mainxfer)")
    ap.add_argument("--keep", action="store_true", help="leave the staging file")
    ap.add_argument("op", choices=["put", "get"])
    ap.add_argument("src")
    ap.add_argument("dst")
    args = ap.parse_args()
    port = args.port or sidecar_xfer.default_port()
    if not port:
        sys.exit("main_xfer: no port (-p, $SIDECAR_PORT or tools/config)")
    main_path = args.dst if args.op == "put" else args.src
    stage = args.stage or f"/sd/{os.path.basename(main_path)}.mainxfer"
    s = sidecar_xfer.open_port(port)
    try:
        if args.op == "put":
            data = open(args.src, "rb").read()
            sha = hashlib.sha256(data).hexdigest()
            sidecar_xfer.put(s, args.src, stage)
            sidecar_cp(s, stage, f"main:{main_path}", len(data))
            got_sha, got_size = main_hash(s, main_path)
        else:
            # The size isn't known yet: allow for main's whole spiffs.
            sidecar_cp(s, f"main:{main_path}", stage, 3_500_000)
            sha, size = main_hash(s, main_path)
            sidecar_xfer.get(s, stage, args.dst)
            data = open(args.dst, "rb").read()
            got_sha, got_size = hashlib.sha256(data).hexdigest(), len(data)
        if got_sha != sha or got_size != len(data):
            sys.exit(f"main_xfer: MISMATCH: {args.src} {len(data) if args.op == 'put' else size} bytes {sha}, "
                     f"{args.dst} {got_size} bytes {got_sha}")
        print(f"main_xfer: {args.src} -> {args.dst}: {got_size} bytes, sha256 {got_sha} matches")
        if not args.keep:
            command(s, f"rm {stage}", [f"Sidecar: rm {stage}: "], 10)
    finally:
        s.close()


if __name__ == "__main__":
    main()
