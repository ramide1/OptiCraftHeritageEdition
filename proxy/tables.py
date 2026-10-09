"""Mapping helpers between 1.2.5 (protocol 29) and 1.8.9 (protocol 47).

Numeric block ids barely moved between 1.2.5 and 1.8 (Mojang froze them;
new blocks only appended new numbers), so the MVP passes id/meta through
and only handles the >255 case via 1.2.5's Add nibble array. A full
remap table for newer targets (1.12+, and especially the 1.13 flattening)
is phase-2 material and lives here when it lands.
"""


def state_to_id_meta(state):
    return (state >> 4) & 0xFFF, state & 0xF


def pack_nibbles(values):
    out = bytearray((len(values) + 1) // 2)
    for i, v in enumerate(values):
        if i & 1:
            out[i >> 1] |= (v & 0xF) << 4
        else:
            out[i >> 1] = v & 0xF
    return bytes(out)


def convert_chunk_sections(bitmask, data, skylight):
    """1.8 section layout -> 1.2.5 ground-up raw (uncompressed) section
    bytes. data: bytes in 1.8 order (per set section: 4096 big-endian
    shorts id<<4|meta, 2048 block light, 2048 sky light if skylight).
    Returns (primary_mask, add_mask, raw_bytes_without_biome)."""
    off = 0
    sections = []  # (sec_index, ids, metas, light, sky)
    for sec in range(16):
        if not (bitmask >> sec) & 1:
            continue
        ids = [0] * 4096
        metas = [0] * 4096
        for i in range(4096):
            v = (data[off] << 8) | data[off + 1]
            off += 2
            ids[i] = v >> 4
            metas[i] = v & 0xF
        light = data[off:off + 2048]
        off += 2048
        if skylight:
            sky = data[off:off + 2048]
            off += 2048
        else:
            sky = b"\xff" * 2048
        sections.append((sec, ids, metas, bytes(light), bytes(sky)))
    if off != len(data):
        raise ValueError("chunk section bytes left over: %d" % (len(data) - off))

    raw = bytearray()
    add_arrays = []  # (sec, packed) appended after the base sections
    add_mask = 0
    for sec, ids, metas, light, sky in sections:
        raw += bytes(b & 0xFF for b in ids)
        raw += pack_nibbles(metas)
        raw += light
        raw += sky
    for sec, ids, _, _, _ in sections:
        if any(b > 255 for b in ids):
            add_mask |= 1 << sec
            add_arrays.append((sec, pack_nibbles([b >> 8 for b in ids])))
    add_arrays.sort()
    for _, packed in add_arrays:
        raw += packed
    return bitmask & 0xFFFF, add_mask, bytes(raw)


# 1.2.5 EntityAction -> 1.8 Entity Action (jump boost always 0 here).
ENTITY_ACTION = {1: 0, 2: 1, 3: 2, 4: 3, 5: 4}

# Fallback when the 1.2.5 client asks for something unmapped.
UNKNOWN_BLOCK_ID = 0
