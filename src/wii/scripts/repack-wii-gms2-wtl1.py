#!/usr/bin/env python3
"""Bake GMS2 (2zoq) 2048-wide TXTR pages into 1:1 WTL2 for Wii GX.

WTL2 tiles are pre-swizzled GX RGB5A3 so the Wii memcpy-uploads them — no PNG
inflate on the emulated CPU (that was the battle hitch).

2048-wide pages cannot upload to GX. The runtime otherwise nearest-neighbor ÷2
then RGB5A3 — pixel fonts turn to mush.

Each 2048×H page is column-stacked (left over right → 1024 × 2H) so TPAGs that
don't cross x=1024 keep their relative layout (fonts, UI). Sprites that span the
midline are 1:1 shelf-packed below the stack. 2048-wide font atlases get their
glyph coords rewritten so they stay 1024-wide after the stack.

Usage:
  tools/.venv-wii-tex/bin/python src/wii/scripts/repack-wii-gms2-wtl1.py \\
      chapter1_windows/data.win -o data.win.wtl1
"""
from __future__ import annotations

import argparse
import bz2
import io
import struct
import sys
from pathlib import Path

from PIL import Image


def u32(b: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<I", b, off)[0]


def sign_extend(val: int, bits: int) -> int:
    mask = 1 << (bits - 1)
    x = (val ^ mask) - mask
    return x & 0xFF


def decode_gm_qoi(data: bytes) -> Image.Image:
    if len(data) < 12 or data[:4] != b"fioq":
        raise ValueError("not fioq")
    width = data[4] | (data[5] << 8)
    height = data[6] | (data[7] << 8)
    length = u32(data, 8)
    pixel_data = data[12 : 12 + length]
    raw = bytearray(width * height * 4)
    index = bytearray(64 * 4)
    pos = 0
    run = 0
    r = g = b = 0
    a = 255
    for out in range(0, len(raw), 4):
        if run > 0:
            run -= 1
        elif pos < len(pixel_data):
            b1 = pixel_data[pos]
            pos += 1
            if (b1 & 0xC0) == 0x00:
                ip = (b1 & 0x3F) << 2
                r, g, b, a = index[ip : ip + 4]
            elif (b1 & 0xE0) == 0x40:
                run = b1 & 0x1F
            elif (b1 & 0xE0) == 0x60:
                b2 = pixel_data[pos]
                pos += 1
                run = (((b1 & 0x1F) << 8) | b2) + 32
            elif (b1 & 0xC0) == 0x80:
                r = (r + sign_extend((b1 >> 4) & 3, 2)) & 0xFF
                g = (g + sign_extend((b1 >> 2) & 3, 2)) & 0xFF
                b = (b + sign_extend(b1 & 3, 2)) & 0xFF
            elif (b1 & 0xE0) == 0xC0:
                b2 = pixel_data[pos]
                pos += 1
                merged = (b1 << 8) | b2
                r = (r + sign_extend((merged >> 8) & 0x1F, 5)) & 0xFF
                g = (g + sign_extend((merged >> 4) & 0x0F, 4)) & 0xFF
                b = (b + sign_extend(merged & 0x0F, 4)) & 0xFF
            elif (b1 & 0xF0) == 0xE0:
                b2 = pixel_data[pos]
                b3 = pixel_data[pos + 1]
                pos += 2
                merged = (b1 << 16) | (b2 << 8) | b3
                r = (r + sign_extend((merged >> 15) & 0x1F, 5)) & 0xFF
                g = (g + sign_extend((merged >> 10) & 0x1F, 5)) & 0xFF
                b = (b + sign_extend((merged >> 5) & 0x1F, 5)) & 0xFF
                a = (a + sign_extend(merged & 0x1F, 5)) & 0xFF
            elif (b1 & 0xF0) == 0xF0:
                if b1 & 8:
                    r = pixel_data[pos]
                    pos += 1
                if b1 & 4:
                    g = pixel_data[pos]
                    pos += 1
                if b1 & 2:
                    b = pixel_data[pos]
                    pos += 1
                if b1 & 1:
                    a = pixel_data[pos]
                    pos += 1
            ip2 = ((r ^ g ^ b ^ a) & 63) << 2
            index[ip2 : ip2 + 4] = bytes((r, g, b, a))
        raw[out : out + 4] = bytes((r, g, b, a))
    return Image.frombytes("RGBA", (width, height), bytes(raw))


def decode_2zoq(blob: bytes, header_size: int = 12) -> Image.Image:
    if blob[:4] != b"2zoq":
        raise ValueError(f"not 2zoq: {blob[:4]!r}")
    uncompressed = bz2.decompress(blob[header_size:])
    return decode_gm_qoi(uncompressed)


def find_chunk(data: bytes | bytearray, tag: bytes) -> tuple[int, int, int]:
    pos = 8
    end = 8 + u32(data, 4)
    while pos + 8 <= end:
        t = bytes(data[pos : pos + 4])
        size = u32(data, pos + 4)
        if t == tag:
            return pos, pos + 8, size
        pos += 8 + size
        if pos & 1:
            pos += 1
    raise SystemExit(f"chunk {tag!r} not found")


def txtr_entries_gms2(data: bytes | bytearray) -> list[dict]:
    _, base, size = find_chunk(data, b"TXTR")
    count = u32(data, base)
    ptrs = [u32(data, base + 4 + i * 4) for i in range(count)]
    if count >= 2 and ptrs[1] - ptrs[0] != 28:
        raise SystemExit(f"expected 2022.9 TXTR entries (28 bytes), got {ptrs[1]-ptrs[0]}")
    out = []
    for i, eoff in enumerate(ptrs):
        w = u32(data, eoff + 12)
        h = u32(data, eoff + 16)
        blob = u32(data, eoff + 24)
        if i + 1 < count:
            bsz = u32(data, ptrs[i + 1] + 24) - blob
        else:
            bsz = (base + size) - blob
        out.append({"entry": eoff, "blob": blob, "size": bsz, "w": w, "h": h})
    return out


def pack_rgb5a3(r: int, g: int, b: int, a: int) -> int:
    # Punch-through (matches gx_renderer). 3-bit alpha made writer/UI boxes see-through.
    if a >= 32:
        return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
    return 0


def encode_gx_rgb5a3(img: Image.Image) -> bytes:
    """GX 4×4-tiled RGB5A3, big-endian halfwords — memcpy-ready for the Wii loader."""
    src = img.convert("RGBA")
    src_w, src_h = src.size
    pw = (src_w + 3) & ~3
    ph = (src_h + 3) & ~3
    if ph > 1024:
        ph = 1024
    px = src.tobytes()
    out = bytearray(pw * ph * 2)
    tiles_x = pw // 4
    tiles_y = ph // 4
    for ty in range(tiles_y):
        for tx in range(tiles_x):
            off = (ty * tiles_x + tx) * 32
            for row in range(4):
                for col in range(4):
                    x = tx * 4 + col
                    y = ty * 4 + row
                    if x < src_w and y < src_h:
                        i = (y * src_w + x) * 4
                        r, g, b, a = px[i], px[i + 1], px[i + 2], px[i + 3]
                    else:
                        r = g = b = a = 0
                    struct.pack_into(">H", out, off + (row * 4 + col) * 2, pack_rgb5a3(r, g, b, a))
    return bytes(out)


def save_wtl2(img: Image.Image, tile_h: int = 1024) -> bytes:
    w, h = img.size
    if w > 1024:
        raise SystemExit(f"WTL2 width {w} exceeds GX cap 1024")
    tiles: list[bytes] = []
    y = 0
    while y < h:
        th = min(tile_h, h - y)
        tiles.append(encode_gx_rgb5a3(img.crop((0, y, w, y + th))))
        y += th
    header_size = 20 + 8 * len(tiles)
    index = bytearray()
    payload = bytearray()
    for tile in tiles:
        while len(payload) % 32:
            payload += b"\x00"
        index += struct.pack("<II", header_size + len(payload), len(tile))
        payload += tile
    return b"WTL2" + struct.pack("<IIII", w, h, tile_h, len(tiles)) + bytes(index) + bytes(payload)


def save_png(img: Image.Image) -> bytes:
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def save_wtl1(img: Image.Image, tile_h: int = 1024) -> bytes:
    w, h = img.size
    if w > 1024:
        raise SystemExit(f"WTL1 width {w} exceeds GX cap 1024")
    tiles: list[bytes] = []
    y = 0
    while y < h:
        th = min(tile_h, h - y)
        tiles.append(save_png(img.crop((0, y, w, y + th))))
        y += th
    header_size = 20 + 8 * len(tiles)
    index = bytearray()
    payload = bytearray()
    for png in tiles:
        while len(payload) % 4:
            payload += b"\x00"
        index += struct.pack("<II", header_size + len(payload), len(png))
        payload += png
    return b"WTL1" + struct.pack("<IIII", w, h, tile_h, len(tiles)) + bytes(index) + bytes(payload)


def collect_font_tpag_offsets(data: bytes | bytearray) -> set[int]:
    """FONT stores a file offset to its TPAG item (resolved later by the runner)."""
    _, fbase, _ = find_chunk(data, b"FONT")
    count = u32(data, fbase)
    offs: set[int] = set()
    for i in range(count):
        foff = u32(data, fbase + 4 + i * 4)
        if foff == 0:
            continue
        # name ptr, display ptr, emSize, bold, italic, rangeStart/charset/aa, rangeEnd, tpag
        offs.add(u32(data, foff + 28))
    return offs


def font_optional_count(data: bytes | bytearray, font_off: int) -> int:
    base_after_scale_y = font_off + 40
    for trial in range(1, 5):
        list_start = base_after_scale_y + 4 * trial
        probed = u32(data, list_start)
        if probed == 0 or probed > 0x10000:
            continue
        first = u32(data, list_start + 4)
        expected = list_start + 4 + 4 * probed
        if first == expected:
            return trial
    return 1


def remap_font_glyphs_column_stack(
    data: bytearray, tpag_off: int, src_h: int
) -> None:
    """Rewrite a 2048-wide font atlas TPAG + glyph coords after column-stack.

    Glyphs are relative to the font TPAG origin. The right half of a 2048-wide
    atlas lands at y += src_h after stacking, so relative X/Y must move with it.
    """
    sx, sy, sw, sh = struct.unpack_from("<HHHH", data, tpag_off)
    _, fbase, _ = find_chunk(data, b"FONT")
    fcount = u32(data, fbase)
    new_abs: list[tuple[int, int, int, int, int]] = []
    for i in range(fcount):
        foff = u32(data, fbase + 4 + i * 4)
        if foff == 0 or u32(data, foff + 28) != tpag_off:
            continue
        opt = font_optional_count(data, foff)
        list_start = foff + 40 + 4 * opt
        gcount = u32(data, list_start)
        for j in range(gcount):
            gp = u32(data, list_start + 4 + j * 4)
            gx, gy, gw, gh = struct.unpack_from("<HHHH", data, gp + 2)
            absx = sx + gx
            absy = sy + gy
            if absx >= 1024:
                absx -= 1024
                absy += src_h
            new_abs.append((gp, absx, absy, gw, gh))
    if not new_abs:
        return
    minx = min(a[1] for a in new_abs)
    miny = min(a[2] for a in new_abs)
    maxx = max(a[1] + a[3] for a in new_abs)
    maxy = max(a[2] + a[4] for a in new_abs)
    nsx, nsy = minx, miny
    nsw = min(1024, max(1, maxx - minx))
    nsh = max(1, maxy - miny)
    struct.pack_into("<HHHH", data, tpag_off, nsx, nsy, nsw, nsh)
    # Keep crop/bounding in sync when they matched the old source rect.
    tw, th, bw, bh = struct.unpack_from("<HHHH", data, tpag_off + 8)
    if tw == sw:
        struct.pack_into("<H", data, tpag_off + 12, nsw)
    if th == sh:
        struct.pack_into("<H", data, tpag_off + 14, nsh)
    if bw == sw:
        struct.pack_into("<H", data, tpag_off + 16, nsw)
    if bh == sh:
        struct.pack_into("<H", data, tpag_off + 18, nsh)
    for gp, absx, absy, _gw, _gh in new_abs:
        struct.pack_into("<HH", data, gp + 2, absx - nsx, absy - nsy)


def collect_page_rects(data: bytes | bytearray, page_id: int) -> list[tuple[int, int, int, int, int]]:
    """Return (tpag_off, sx, sy, sw, sh) for items on page_id."""
    _, tbase, _ = find_chunk(data, b"TPAG")
    count = u32(data, tbase)
    out = []
    for i in range(count):
        off = u32(data, tbase + 4 + i * 4)
        sx, sy, sw, sh = struct.unpack_from("<HHHH", data, off)
        page = struct.unpack_from("<h", data, off + 20)[0]
        if page != page_id or sw == 0 or sh == 0:
            continue
        out.append((off, sx, sy, sw, sh))
    return out


def shelf_pack(
    items: list[tuple[object, int, int]], max_w: int, max_h: int, pad: int = 1
) -> tuple[dict[object, tuple[int, int]], int, int]:
    order = sorted(items, key=lambda t: (-t[2], -t[1]))
    shelves: list[list[int]] = []
    places: dict[object, tuple[int, int]] = {}
    atlas_h = 0
    for key, w, h in order:
        if w > max_w:
            raise RuntimeError(f"item wider than atlas: {key!r} ({w}x{h} > {max_w})")
        w2, h2 = w + pad, h + pad
        placed = False
        for sh in shelves:
            y, sh_h, x = sh
            if h2 <= sh_h and x + w2 <= max_w:
                places[key] = (x, y)
                sh[2] = x + w2
                placed = True
                break
        if placed:
            continue
        y = atlas_h
        if y + h2 > max_h:
            raise RuntimeError(f"atlas overflow packing {key!r} ({w}x{h})")
        places[key] = (0, y)
        shelves.append([y, h2, w2])
        atlas_h = y + h2
    ah = max(4, (atlas_h + 3) & ~3)
    return places, max_w, min(max_h, ah)


def pack_page_1to1(img: Image.Image, rects: list[tuple[int, int, int, int, int]]) -> tuple[Image.Image, dict[tuple[int, int, int, int], tuple[int, int]]]:
    uniq: dict[tuple[int, int, int, int], list[int]] = {}
    for off, sx, sy, sw, sh in rects:
        uniq.setdefault((sx, sy, sw, sh), []).append(off)
    items = [(key, key[2], key[3]) for key in uniq]
    last_err = ""
    for pad in (1, 0):
        try:
            places, aw, ah = shelf_pack(items, 1024, 16384, pad=pad)
            break
        except RuntimeError as e:
            last_err = str(e)
            places = None
    if not places:
        raise SystemExit(f"1:1 shelf pack failed: {last_err}")
    atlas = Image.new("RGBA", (aw, ah), (0, 0, 0, 0))
    for (sx, sy, sw, sh), (x, y) in places.items():
        atlas.paste(img.crop((sx, sy, sx + sw, sy + sh)), (x, y))
    return atlas, places


def column_stack_2048(img: Image.Image) -> Image.Image:
    w, h = img.size
    if w != 2048:
        raise SystemExit(f"expected 2048-wide, got {w}x{h}")
    left = img.crop((0, 0, 1024, h))
    right = img.crop((1024, 0, 2048, h))
    out = Image.new("RGBA", (1024, h * 2), (0, 0, 0, 0))
    out.paste(left, (0, 0))
    out.paste(right, (0, h))
    return out


def remap_tpag_column_stack(data: bytearray, rects: list[tuple[int, int, int, int, int]], src_h: int) -> int:
    spanned = 0
    for off, sx, sy, sw, sh in rects:
        if sx < 1024 < sx + sw:
            spanned += 1
            continue
        if sx >= 1024:
            nsx = sx - 1024
            nsy = sy + src_h
        else:
            nsx, nsy = sx, sy
        struct.pack_into("<HH", data, off, nsx, nsy)
    return spanned


def rewrite_blobs_gms2(data: bytearray, new_blobs: dict[int, bytes], new_wh: dict[int, tuple[int, int]]) -> int:
    txtr_hdr, txtr_base, txtr_size = find_chunk(data, b"TXTR")
    textures = txtr_entries_gms2(data)
    first_blob = min(t["blob"] for t in textures if t["blob"] != 0)
    old_end = txtr_base + txtr_size

    rebuilt: list[bytes] = []
    for i, tex in enumerate(textures):
        if i in new_blobs:
            rebuilt.append(new_blobs[i])
        else:
            rebuilt.append(bytes(data[tex["blob"] : tex["blob"] + tex["size"]]))

    payload = bytearray()
    offsets: list[int] = []
    for blob in rebuilt:
        while len(payload) % 4:
            payload += b"\x00"
        offsets.append(first_blob + len(payload))
        payload += blob

    new_end = first_blob + len(payload)
    delta = new_end - old_end
    if delta > 0:
        data[old_end:old_end] = b"\x00" * delta
    elif delta < 0:
        del data[new_end:old_end]

    data[first_blob : first_blob + len(payload)] = payload
    struct.pack_into("<I", data, txtr_hdr + 4, new_end - txtr_base)

    for i, tex in enumerate(textures):
        struct.pack_into("<I", data, tex["entry"] + 24, offsets[i])
        if i in new_wh:
            nw, nh = new_wh[i]
            struct.pack_into("<II", data, tex["entry"] + 12, nw, nh)

    if delta != 0:
        audo_hdr = new_end
        if data[audo_hdr : audo_hdr + 4] != b"AUDO":
            raise SystemExit(
                f"expected AUDO after TXTR at {audo_hdr}, found {data[audo_hdr:audo_hdr+4]!r}"
            )
        audo_base = audo_hdr + 8
        audo_count = u32(data, audo_base)
        for i in range(audo_count):
            poff = audo_base + 4 + i * 4
            ptr = u32(data, poff)
            if ptr != 0:
                struct.pack_into("<I", data, poff, ptr + delta)

    struct.pack_into("<I", data, 4, len(data) - 8)
    return delta


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    src = Path(args.src)
    orig = bytearray(src.read_bytes())
    textures = txtr_entries_gms2(orig)
    new_blobs: dict[int, bytes] = {}
    new_wh: dict[int, tuple[int, int]] = {}
    font_tpags = collect_font_tpag_offsets(orig)

    for i, tex in enumerate(textures):
        if tex["w"] <= 1024:
            print(f"page {i}: {tex['w']}x{tex['h']} already GX-legal, keep 2zoq")
            continue
        if tex["w"] != 2048:
            print(f"page {i}: unsupported {tex['w']}x{tex['h']}, skip", file=sys.stderr)
            continue
        blob = bytes(orig[tex["blob"] : tex["blob"] + tex["size"]])
        print(f"page {i}: decode 2zoq {tex['w']}x{tex['h']} ({len(blob)} bytes)...")
        img = decode_2zoq(blob)
        if img.size != (tex["w"], tex["h"]):
            raise SystemExit(f"page {i}: decoded {img.size} != metadata {tex['w']}x{tex['h']}")
        rects = collect_page_rects(orig, i)
        src_h = tex["h"]
        stacked = column_stack_2048(img)
        unspan = [r for r in rects if not (r[1] < 1024 < r[1] + r[3])]
        span = [r for r in rects if r[1] < 1024 < r[1] + r[3]]
        font_span = [r for r in span if r[0] in font_tpags]
        other_span = [r for r in span if r[0] not in font_tpags]
        remap_tpag_column_stack(orig, unspan, src_h)
        for off, _sx, _sy, _sw, _sh in font_span:
            remap_font_glyphs_column_stack(orig, off, src_h)
        atlas = stacked
        how = f"column-stack ({len(span)} span, {len(font_span)} font)"
        if other_span:
            extra, places = pack_page_1to1(img, other_span)
            stacked_h = stacked.size[1]
            atlas = Image.new("RGBA", (1024, stacked_h + extra.size[1]), (0, 0, 0, 0))
            atlas.paste(stacked, (0, 0))
            atlas.paste(extra, (0, stacked_h))
            for off, sx, sy, sw, sh in other_span:
                nx, ny = places[(sx, sy, sw, sh)]
                struct.pack_into("<HH", orig, off, nx, ny + stacked_h)
            how = f"column-stack+span-shelf ({len(other_span)} extra, {len(font_span)} font)"
        wtl = save_wtl2(atlas)
        new_blobs[i] = wtl
        new_wh[i] = atlas.size
        print(f"  -> {how} WTL2 {atlas.size[0]}x{atlas.size[1]} ({len(wtl)} bytes), {len(rects)} TPAGs")

    delta = rewrite_blobs_gms2(orig, new_blobs, new_wh)
    out = Path(args.output)
    out.write_bytes(orig)
    print(f"wrote {out} ({len(orig)} bytes, TXTR delta {delta:+d})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
