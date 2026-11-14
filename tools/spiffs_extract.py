#!/usr/bin/env python3
"""List or extract files from a SPIFFS image (a partition dump).

For the Sidecar's spiffs: read it with esptool read_flash, then pull files
out of it here (a fetched core dump, for one). The format is IDF's SPIFFS
with this project's settings (sidecar/flash_spiffs.sh, spiffs/Makefile):
4096-byte blocks, 256-byte pages, 32-byte names, 4 bytes of metadata, no
magic, 2-byte object ids/span and page indexes, little endian. It reads
images written by the SPIFFS library at run time, not just spiffsgen.py's:
a file is its live object index pages (span 0 has the name and size), and
they list its data pages in order.

Usage:
  spiffs_extract.py IMAGE                    list the files
  spiffs_extract.py IMAGE NAME OUT           extract NAME (e.g. /core/main.core) to OUT
  spiffs_extract.py IMAGE --all DIR          extract everything under DIR
Exit status: 0 ok, 1 file not found or damaged, 2 usage.
"""
import os
import struct
import sys

BLOCK = 4096
PAGE = 256
NAME_LEN = 32
META_LEN = 4
PAGES_PER_BLOCK = BLOCK // PAGE
LU_PAGES = -(-PAGES_PER_BLOCK * 2 // PAGE)   # object lookup pages at each block's start: 1
PH_LEN = 5                                   # obj_id u16, span_ix u16, flags u8
DATA_LEN = PAGE - PH_LEN                     # 251 bytes of data per data page
IX_HDR_LEN = 8 + 4 + 1 + NAME_LEN + META_LEN # span 0: header + align, size, type, name, meta
IX_HEAD_LIM = (PAGE - IX_HDR_LEN) // 2       # data page indexes in span 0's index page
IX_LIM = (PAGE - 8) // 2                     # ... in the later ones
IX_FLAG = 0x8000                             # an object index page's id has the top bit set

# Page header flags: a flag is set when its bit is 0.
F_USED, F_FINAL, F_INDEX, F_DELET = 0x01, 0x02, 0x04, 0x80


def pages(img):
    """(page index, obj_id, span_ix, flags) for every live page."""
    for b in range(len(img) // BLOCK):
        for p in range(LU_PAGES, PAGES_PER_BLOCK):
            pix = b * PAGES_PER_BLOCK + p
            obj_id, span, flags = struct.unpack_from('<HHB', img, pix * PAGE)
            if obj_id in (0xFFFF, 0x0000) or obj_id == IX_FLAG:
                continue   # free, or deleted
            if flags & F_USED or flags & F_FINAL or not flags & F_DELET:
                continue   # not written through, or deleted
            yield pix, obj_id, span, flags


def files(img):
    """{name: (size, [data page index, ...])}."""
    index = {}   # obj_id (without IX_FLAG) -> {span: page index}
    data = {}    # (obj_id, data span) -> page index
    for pix, obj_id, span, flags in pages(img):
        if not flags & F_INDEX and obj_id & IX_FLAG:
            index.setdefault(obj_id & ~IX_FLAG, {})[span] = pix
        elif flags & F_INDEX and not obj_id & IX_FLAG:
            data[(obj_id, span)] = pix
    out = {}
    for obj_id, spans in index.items():
        if 0 not in spans:
            continue
        head = spans[0] * PAGE
        size, typ = struct.unpack_from('<IB', img, head + 8)
        name = img[head + 13:head + 13 + NAME_LEN].split(b'\0', 1)[0].decode(errors='replace')
        if typ != 1:
            continue   # not a file
        if size == 0xFFFFFFFF:
            size = 0   # never closed with a size
        npages = -(-size // DATA_LEN)
        plist = []
        for k in range(npages):
            if k < IX_HEAD_LIM:
                ix_span, slot, base = 0, k, IX_HDR_LEN
            else:
                ix_span = 1 + (k - IX_HEAD_LIM) // IX_LIM
                slot, base = (k - IX_HEAD_LIM) % IX_LIM, 8
            ixp = spans.get(ix_span)
            pix = None
            if ixp is not None:
                pix = struct.unpack_from('<H', img, ixp * PAGE + base + 2 * slot)[0]
                if pix == 0xFFFF:
                    pix = None
            if pix is None:
                pix = data.get((obj_id, k))   # fall back to the data page's own header
            plist.append(pix)
        out[name] = (size, plist, obj_id)
    return out


def read_file(img, size, plist, obj_id):
    """The file's bytes, or None if a data page is missing or isn't this file's."""
    chunks = []
    for k, pix in enumerate(plist):
        if pix is None:
            return None
        oid, span, flags = struct.unpack_from('<HHB', img, pix * PAGE)
        if oid != obj_id or span != k:
            return None
        chunks.append(img[pix * PAGE + PH_LEN:(pix + 1) * PAGE])
    return b''.join(chunks)[:size]


def main(argv):
    if len(argv) < 2 or (len(argv) > 2 and len(argv) != 4):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    img = open(argv[1], 'rb').read()
    fs = files(img)
    if len(argv) == 2:
        for name in sorted(fs):
            size, plist, obj_id = fs[name]
            ok = read_file(img, size, plist, obj_id) is not None
            print(f'{size:9d}  {name}{"" if ok else "  (DAMAGED)"}')
        return 0
    if argv[2] == '--all':
        status = 0
        for name, (size, plist, obj_id) in fs.items():
            data = read_file(img, size, plist, obj_id)
            if data is None:
                print(f'spiffs_extract: {name}: damaged, skipped', file=sys.stderr)
                status = 1
                continue
            path = os.path.join(argv[3], name.lstrip('/'))
            os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
            open(path, 'wb').write(data)
        return status
    name = argv[2]
    if name not in fs and '/' + name in fs:
        name = '/' + name
    if name not in fs:
        print(f'spiffs_extract: {argv[2]}: not in the image', file=sys.stderr)
        return 1
    size, plist, obj_id = fs[name]
    data = read_file(img, size, plist, obj_id)
    if data is None:
        print(f'spiffs_extract: {name}: damaged (a data page missing or not its own)', file=sys.stderr)
        return 1
    open(argv[3], 'wb').write(data)
    print(f'spiffs_extract: {name}: {size} bytes -> {argv[3]}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
