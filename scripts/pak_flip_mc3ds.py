#!/usr/bin/env python3
"""Converts the texture orientation convention of assets inside an
OptiCraft assets.pak between Minecraft 3DS Edition and Java Edition.

Minecraft 3DS Edition stores its art upside down relative to Java: plain
images are vertically flipped, and skins are flipped on both axes (the
reason github.com/Cracko298/MC-3DS-Flip exists, and the reason art taken
from the MC-3DS ecosystem renders upside down in this port). This script
applies those flips to the matching entries of a pak, so a pak whose art
came from MC-3DS renders correctly in OptiCraft -- which addresses
textures in the Java convention.

A flip is its own inverse, so running the script again converts back.

Usage:
    pak_flip_mc3ds.py <assets.pak> <output.pak>
        flips the default MC-3DS-derived set (see below)
    pak_flip_mc3ds.py <in.pak> <out.pak> --all
        flips EVERY PNG in the pak: Y for plain images, X+Y for skins.
        Use this only when the whole pack came from the MC-3DS side --
        flipping a mixed pack breaks the Java-convention half (terrain,
        gui, font), and a flip is its own inverse: never run it twice on
        the same art.
    pak_flip_mc3ds.py <in.pak> <out.pak> --yflip PATTERN [--yflip ...]
                                             --xyflip PATTERN [--xyflip ...]
        flips only the entries whose pak key matches a pattern (fnmatch,
        case-insensitive, forward slashes: "assets/skins/*.png")
    ... --skip PATTERN [--skip ...]
        exempts matching entries from every flip -- for building a
        uniformly-oriented pak out of a mixed one: flip what is still in
        the other convention, skip what already arrived converted.
    ... --dry-run
        lists what would be flipped and exits

Defaults (used when no --yflip/--xyflip/--all is given):
    --yflip  assets/legacy/title.png      the legacy menu title
    --xyflip assets/skins/*.png           every skin (full sheet, _32 and
                                          _Front thumbnails alike)

Only 8-bit non-interlaced PNGs are flipped; anything else that matches a
pattern is left untouched with a warning. Non-PNG entries are copied
verbatim, and the output pak is byte-format identical to what
scripts/make_pak.py builds.
"""

import fnmatch
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

MAGIC = b"MCPK"
VERSION = 1
HEADER_BYTES = 32
ENTRY_BYTES = 16
DATA_ALIGN = 64

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

DEFAULT_YFLIP = ("assets/legacy/title.png",)
DEFAULT_XYFLIP = ("assets/skins/*.png",)


class SkipPng(Exception):
    """The image is a PNG this flipper cannot rewrite; leave it verbatim."""


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


def chunk_crc(chunk_type, payload):
    return zlib.crc32(chunk_type + payload) & 0xFFFFFFFF


def append_chunk(out, chunk_type, payload):
    out += struct.pack(">I", len(payload))
    out += chunk_type
    out += payload
    out += struct.pack(">I", chunk_crc(chunk_type, payload))


def parse_png(data):
    if not data.startswith(PNG_SIGNATURE):
        raise SkipPng("not a PNG")
    chunks = []
    cursor = len(PNG_SIGNATURE)
    while cursor + 8 <= len(data):
        length, = struct.unpack(">I", data[cursor:cursor + 4])
        chunk_type = data[cursor + 4:cursor + 8]
        payload = data[cursor + 8:cursor + 8 + length]
        if len(payload) != length:
            raise SkipPng("truncated chunk")
        cursor += 12 + length
        chunks.append((chunk_type, payload))
        if chunk_type == b"IEND":
            break
    if not chunks or chunks[0][0] != b"IHDR":
        raise SkipPng("missing IHDR")

    width, height, bit_depth, color_type, _, _, interlace = struct.unpack(
        ">IIBBBBB", chunks[0][1])
    if bit_depth not in (1, 2, 4, 8):
        raise SkipPng("bit depth %d (1/2/4/8 supported)" % bit_depth)
    if interlace != 0:
        raise SkipPng("interlaced (Adam7 not supported)")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color_type)
    if channels is None:
        raise SkipPng("color type %d" % color_type)

    idat = bytearray()
    palette = None
    transparency = None
    for chunk_type, payload in chunks:
        if chunk_type == b"IDAT":
            idat += payload
        elif chunk_type == b"PLTE":
            palette = payload
        elif chunk_type == b"tRNS":
            transparency = payload
    if not idat:
        raise SkipPng("no IDAT")
    return width, height, bit_depth, color_type, channels, palette, transparency, bytes(idat)


def unfilter_scanlines(raw, stride, height, filter_bpp):
    """PNG unfiltering works on bytes, not pixels: filter_bpp is the number
    of bytes one filtering unit spans (max(1, bits_per_pixel // 8)), so
    sub-byte depths -- 1-bit palette fonts among them -- unfilter with the
    same arithmetic as 8-bit images; only the stride differs."""
    rows = []
    previous = bytearray(stride)
    cursor = 0
    for _ in range(height):
        if cursor + 1 + stride > len(raw):
            raise SkipPng("image data short")
        filter_type = raw[cursor]
        scanline = bytearray(raw[cursor + 1:cursor + 1 + stride])
        cursor += 1 + stride
        if filter_type == 0:
            pass
        elif filter_type == 1:  # Sub
            for i in range(filter_bpp, stride):
                scanline[i] = (scanline[i] + scanline[i - filter_bpp]) & 0xFF
        elif filter_type == 2:  # Up
            for i in range(stride):
                scanline[i] = (scanline[i] + previous[i]) & 0xFF
        elif filter_type == 3:  # Average
            for i in range(stride):
                left = scanline[i - filter_bpp] if i >= filter_bpp else 0
                scanline[i] = (scanline[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:  # Paeth
            for i in range(stride):
                left = scanline[i - filter_bpp] if i >= filter_bpp else 0
                up_left = previous[i - filter_bpp] if i >= filter_bpp else 0
                predictor = left + previous[i] - up_left
                pa = abs(predictor - left)
                pb = abs(predictor - previous[i])
                pc = abs(predictor - up_left)
                if pa <= pb and pa <= pc:
                    predictor = left
                elif pb <= pc:
                    predictor = previous[i]
                else:
                    predictor = up_left
                scanline[i] = (scanline[i] + predictor) & 0xFF
        else:
            raise SkipPng("filter type %d" % filter_type)
        rows.append(bytes(scanline))
        previous = scanline
    return rows


def flip_png(data, flip_x, flip_y):
    """Returns a new PNG with the requested axis flips applied."""
    width, height, bit_depth, color_type, channels, palette, transparency, idat = parse_png(data)
    stride = (width * bit_depth * channels + 7) // 8
    filter_bpp = max(1, (bit_depth * channels) // 8)
    rows = unfilter_scanlines(zlib.decompress(idat), stride, height, filter_bpp)

    if flip_y:
        # Scanline order only: no pixel decoding needed, so every bit depth
        # and palette combination flips vertically here.
        rows.reverse()
    if flip_x:
        if bit_depth != 8:
            # Pixel-exact X flipping would need repacking sub-byte rows;
            # nothing in the MC-3DS set needs it (skins are 8-bit).
            raise SkipPng("X flip needs 8-bit pixels (got %d)" % bit_depth)
        row_pixels = width * channels
        flipped = []
        for row in rows:
            pixels = [row[i:i + channels] for i in range(0, row_pixels, channels)]
            pixels.reverse()
            flipped.append(b"".join(pixels))
        rows = flipped

    out = bytearray(PNG_SIGNATURE)
    ihdr = struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, 0)
    append_chunk(out, b"IHDR", ihdr)
    if palette is not None:
        append_chunk(out, b"PLTE", palette)
    if transparency is not None:
        append_chunk(out, b"tRNS", transparency)
    idat_bytes = bytearray()
    for row in rows:
        idat_bytes.append(0)  # filter type None for every scanline
        idat_bytes += row
    append_chunk(out, b"IDAT", zlib.compress(bytes(idat_bytes)))
    append_chunk(out, b"IEND", b"")
    return bytes(out)


def fnv1a32(text):
    value = 0x811C9DC5
    for byte in text.encode("utf-8"):
        value ^= byte
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def write_pak(entries, output):
    """entries: list of (key, payload). Same layout as make_pak.py."""
    names = bytearray()
    records = []
    for key, payload in entries:
        name_offset = len(names)
        names += key.encode("utf-8") + b"\0"
        records.append([fnv1a32(key.lower()), name_offset, 0, len(payload), key, payload])
    records.sort(key=lambda record: (record[0], record[4]))

    table_offset = HEADER_BYTES
    names_offset = table_offset + len(records) * ENTRY_BYTES
    data_offset = align(names_offset + len(names), DATA_ALIGN)

    cursor = data_offset
    for record in records:
        record[2] = cursor
        cursor = align(cursor + record[3], DATA_ALIGN)

    with open(output, "wb") as out:
        out.write(struct.pack(">4sIIIIIII", MAGIC, VERSION, len(records), table_offset,
                              names_offset, len(names), DATA_ALIGN, 0))
        for hash_value, name_offset, offset, size, _, _ in records:
            out.write(struct.pack(">IIII", hash_value, name_offset, offset, size))
        out.write(bytes(names))
        out.write(b"\0" * (data_offset - out.tell()))
        for _, _, offset, _, _, payload in records:
            out.write(b"\0" * (offset - out.tell()))
            out.write(payload)
            out.write(b"\0" * (align(offset + len(payload), DATA_ALIGN)
                               - (offset + len(payload))))


def main():
    args = sys.argv[1:]
    dry_run = False
    flip_all = False
    yflip = []
    xyflip = []
    skip = []
    positional = []
    i = 0
    while i < len(args):
        arg = args[i]
        if arg == "--yflip":
            yflip.append(args[i + 1])
            i += 2
        elif arg == "--xyflip":
            xyflip.append(args[i + 1])
            i += 2
        elif arg == "--skip":
            skip.append(args[i + 1])
            i += 2
        elif arg == "--dry-run":
            dry_run = True
            i += 1
        elif arg == "--all":
            flip_all = True
            i += 1
        else:
            positional.append(arg)
            i += 1
    if len(positional) != 2:
        raise SystemExit(__doc__)
    pak_path, out_path = positional

    if flip_all:
        # Every PNG gets the plain-image flip unless a skin pattern claims
        # it first (the match order in the loop below checks xyflip first).
        yflip.insert(0, "*")
        if not xyflip:
            xyflip = list(DEFAULT_XYFLIP)
    elif not yflip and not xyflip:
        yflip = list(DEFAULT_YFLIP)
        xyflip = list(DEFAULT_XYFLIP)

    def match(key, patterns):
        lowered = key.lower()
        return any(fnmatch.fnmatch(lowered, pattern.lower()) for pattern in patterns)

    entries = []
    flipped = 0
    skipped = 0
    for key, payload in read_pak(pak_path):
        exempt = match(key, skip)
        flip_x = not exempt and match(key, xyflip)
        flip_y = flip_x or (not exempt and match(key, yflip))
        if exempt and (match(key, yflip) or match(key, xyflip)):
            print("skipped %s (matches --skip)" % key)
        elif (flip_x or flip_y) and not dry_run:
            try:
                payload = flip_png(payload, flip_x, flip_y)
                flipped += 1
                print("flipped %s (%s)" % (key, "X+Y" if flip_x else "Y"))
            except SkipPng as reason:
                skipped += 1
                print("LEFT AS-IS %s (%s)" % (key, reason))
        elif flip_x or flip_y:
            print("would flip %s (%s)" % (key, "X+Y" if flip_x else "Y"))
        entries.append((key, payload))

    if dry_run:
        print("dry run: %d entries inspected, nothing written" % len(entries))
        return

    write_pak(entries, out_path)
    print("wrote %s: %d entries, %d flipped, %d left as-is"
          % (out_path, len(entries), flipped, skipped))


if __name__ == "__main__":
    main()
