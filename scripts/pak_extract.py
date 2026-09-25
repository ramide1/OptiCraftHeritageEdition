#!/usr/bin/env python3
"""Extracts an assets.pak built by make_pak.py back into a data/ tree.

Usage:
    pak_extract.py                     opens a file picker for the pak
    pak_extract.py <assets.pak>        extracts next to it, into <dir>/data
    pak_extract.py <assets.pak> <dir>  extracts into <dir> (the tree shape
                                      make_pak.py packs from)

The inverse of make_pak.py: every entry becomes a file at its key path
under the output directory, so the result can be edited and pushed back
with make_pak.py unchanged. The format is the MCPK one (see
src/platform/storage/PakArchive.h): all integers big-endian u32,

    header   32 bytes: 'MCPK', version 1, entryCount, tableOffset,
                       namesOffset, namesBytes, dataAlign, reserved
    table    entryCount x 16 bytes: hash, nameOffset, dataOffset, size
    names    NUL-terminated keys, referenced by nameOffset
    data     each file at a dataAlign boundary, uncompressed
"""

import os
import struct
import sys

MAGIC = b"MCPK"
HEADER_BYTES = 32
ENTRY_BYTES = 16


def read_pak(path):
    """Yields (key, payload bytes) for every entry, in table order."""
    with open(path, "rb") as pak:
        header = pak.read(HEADER_BYTES)
        if len(header) != HEADER_BYTES:
            raise SystemExit("%s: too short for an MCPK header" % path)
        magic, version, count, table_offset, names_offset, names_bytes, data_align, _ = (
            struct.unpack(">4sIIIIIII", header))
        if magic != MAGIC:
            raise SystemExit("%s: not an MCPK pak (magic %r)" % (path, magic))
        if version != 1:
            raise SystemExit("%s: MCPK version %d not supported (expected 1)"
                             % (path, version))
        if data_align <= 0:
            raise SystemExit("%s: nonsensical dataAlign %d" % (path, data_align))

        pak.seek(names_offset)
        names_blob = pak.read(names_bytes)
        if len(names_blob) != names_bytes:
            raise SystemExit("%s: names region truncated" % path)

        for i in range(count):
            pak.seek(table_offset + i * ENTRY_BYTES)
            _, name_offset, data_offset, size = struct.unpack(">IIII", pak.read(ENTRY_BYTES))
            end = names_blob.find(b"\0", name_offset)
            key = names_blob[name_offset:end].decode("utf-8")
            pak.seek(data_offset)
            payload = pak.read(size)
            if len(payload) != size:
                raise SystemExit("%s: entry %s truncated" % (path, key))
            yield key, payload


def main():
    args = sys.argv[1:]
    if not args:
        try:
            import tkinter as tk
            from tkinter import filedialog
        except ImportError:
            raise SystemExit("no path given and tkinter is unavailable; "
                             "usage: pak_extract.py <assets.pak> [outdir]")
        root = tk.Tk()
        root.withdraw()
        pak_path = filedialog.askopenfilename(title="Select assets.pak")
        root.destroy()
        if not pak_path:
            raise SystemExit("no pak chosen")
        out_dir = os.path.join(os.path.dirname(os.path.abspath(pak_path)), "data")
    elif len(args) == 1:
        pak_path = args[0]
        out_dir = os.path.join(os.path.dirname(os.path.abspath(pak_path)), "data")
    elif len(args) == 2:
        pak_path, out_dir = args
    else:
        raise SystemExit("usage: pak_extract.py <assets.pak> [outdir]")

    if os.path.exists(out_dir) and os.listdir(out_dir):
        raise SystemExit("%s already holds files; refusing to overwrite" % out_dir)

    count = 0
    for key, payload in read_pak(pak_path):
        dest = os.path.join(out_dir, *key.split("/"))
        directory = os.path.dirname(dest)
        if directory:
            os.makedirs(directory, exist_ok=True)
        with open(dest, "wb") as out:
            out.write(payload)
        count += 1
    print("extracted %d entries to %s" % (count, out_dir))


if __name__ == "__main__":
    main()
