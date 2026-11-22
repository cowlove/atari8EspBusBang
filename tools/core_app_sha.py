#!/usr/bin/env python3
"""Print the app ELF SHA-256 prefix an ESP-IDF core dump was taken from.

The core's ESP_CORE_DUMP_INFO note holds it as a hex string, as long as
the app's CONFIG_APP_RETRIEVE_LEN_ELF_SHA (9 here); esp-coredump refuses to
decode the core against an ELF whose SHA-256 doesn't start with it.

Usage: core_app_sha.py <core>   (raw partition image or ELF, uncompressed)
Prints the hex prefix; exits 1 if the core has no such note.
"""
import struct
import sys

NAME = b"ESP_CORE_DUMP_INFO\0"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    data = open(sys.argv[1], "rb").read()
    pos = data.find(NAME)
    while pos >= 0:
        # An ELF note: namesz, descsz, type, then the name padded to 4, then
        # the desc: a 32-bit version and the SHA as a 64-byte hex string.
        if pos >= 12:
            namesz, descsz, _ = struct.unpack_from("<III", data, pos - 12)
            if namesz == len(NAME) and descsz >= 4 + 64:
                desc = pos + ((namesz + 3) & ~3) + 4
                sha = data[desc:desc + 64].rstrip(b"\0")
                if sha and all(c in b"0123456789abcdef" for c in sha):
                    print(sha.decode())
                    return
        pos = data.find(NAME, pos + 1)
    sys.exit(1)


if __name__ == "__main__":
    main()
