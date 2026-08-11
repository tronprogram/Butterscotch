#!/usr/bin/env python3
"""Repack wide TXTR atlases for Wii at full sprite resolution (WTL1).

Problem:
  Any 2048-wide / 2048² TXTR (~8–16MB RGBA decode peak) OOMs or thrash on Wii.
  A flat offline ÷2 keeps GX happy but makes Asriel/FloweyX/faces/props crunchy.

Solution:
  Rebuild every originally-wide page as a **1024 × tall** atlas at **1:1**,
  stored as **WTL1** (tiled PNGs). The GX loader decodes one ≤1024-tall tile at
  a time (~4MB peak) and vertical-slices for drawing.

Default pages (all pristine width>1024): 1, 11, 14, 20, 23, 25
  - 1:  Asriel god-form / hyperdeath (+ afinal)
  - 11: Waterfall/Hotland props
  - 14: Omega Flowey (floweyx_*)
  - 20: JP font preload
  - 23/25: dialog faces + battle sprites

Usage:
  ./src/wii/scripts/build-wii-data-win.sh
"""
from __future__ import annotations

import argparse
import io
import struct
import sys
from pathlib import Path

from PIL import Image


def u32(b: bytes | bytearray, off: int) -> int:
    return struct.unpack_from("<I", b, off)[0]


def find_chunk(data: bytes | bytearray, tag: bytes) -> tuple[int, int, int]:
    pos = 8
    end = 8 + u32(data, 4)
    while pos + 8 <= end:
        t = bytes(data[pos : pos + 4])
        size = u32(data, pos + 4)
        if t == tag:
            return pos, pos + 8, size
        pos += 8 + size
    raise SystemExit(f"chunk {tag!r} not found")


def txtr_textures(data: bytes | bytearray) -> list[dict]:
    _, base, size = find_chunk(data, b"TXTR")
    count = u32(data, base)
    ptrs = [u32(data, base + 4 + i * 4) for i in range(count)]
    out: list[dict] = []
    for i, eoff in enumerate(ptrs):
        blob = u32(data, eoff + 4)
        if i + 1 < count:
            bsz = u32(data, ptrs[i + 1] + 4) - blob
        else:
            bsz = (base + size) - blob
        out.append({"entry": eoff, "blob": blob, "size": bsz})
    return out


def load_png_from_slot(data: bytes, blob: int, bsz: int) -> Image.Image:
    raw = data[blob : blob + bsz]
    iend = raw.find(b"IEND")
    png = raw[: iend + 8] if iend >= 4 else raw
    return Image.open(io.BytesIO(png)).convert("RGBA")


def save_png(img: Image.Image) -> bytes:
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


# Wii tiled atlas container: full-res 1024×N art without a single huge PNG decode.
# Magic "WTL1" + dims + per-tile PNG offsets. Loader decodes one ≤1024-tall tile at a time.
WTL1_MAGIC = b"WTL1"
WTL1_TILE_H = 1024


def save_wtl1(img: Image.Image, tile_h: int = WTL1_TILE_H) -> bytes:
    """Pack atlas as WTL1 (tiled PNGs). Peak decode on Wii ≈ one tile (≤1024² RGBA)."""
    w, h = img.size
    if w > 1024:
        raise SystemExit(f"WTL1 width {w} exceeds GX cap 1024")
    tiles_png: list[bytes] = []
    y = 0
    while y < h:
        th = min(tile_h, h - y)
        tiles_png.append(save_png(img.crop((0, y, w, y + th))))
        y += th

    header_size = 20 + 8 * len(tiles_png)
    index = bytearray()
    payload = bytearray()
    for i, png in enumerate(tiles_png):
        # pad payload so each tile starts 4-byte aligned
        while len(payload) % 4:
            payload += b"\x00"
        index += struct.pack("<II", header_size + len(payload), len(png))
        payload += png

    return WTL1_MAGIC + struct.pack("<IIII", w, h, tile_h, len(tiles_png)) + bytes(index) + bytes(payload)


def rewrite_txtr_blobs(data: bytearray, new_blobs: dict[int, bytes]) -> int:
    """Replace TXTR page blobs; grow/shrink the TXTR chunk and shift AUDO if needed.

    Undertale 1.0 derives each page's blobSize from successive absolute blobOffsets
    (last page → end of TXTR). Dense 1:1 WTL1 packs can exceed the pristine PNG
    slot (notably Omega Flowey page 14) — grow the chunk rather than ÷2 sprites.

    Returns net byte delta applied to the file (AUDO shift).
    """
    if not new_blobs:
        return 0

    txtr_hdr, txtr_base, txtr_size = find_chunk(data, b"TXTR")
    textures = txtr_textures(data)
    count = len(textures)
    first_blob = textures[0]["blob"]
    old_end = txtr_base + txtr_size  # absolute offset of next chunk (AUDO)

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
    # Undertale 1.0 TXTR entry: u32 scaled, u32 blobOffset
    for i, tex in enumerate(textures):
        struct.pack_into("<I", data, tex["entry"] + 4, offsets[i])

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


def shelf_pack(
    items: list[tuple[object, int, int]], max_w: int, max_h: int, pad: int = 1
) -> tuple[dict[object, tuple[int, int]], int, int]:
    """Pack (key, w, h) into atlas; returns places, atlas_w, atlas_h."""
    order = sorted(items, key=lambda t: (-t[2], -t[1]))
    shelves: list[list[int]] = []  # y, height, x_cursor
    places: dict[object, tuple[int, int]] = {}
    atlas_h = 0
    for key, w, h in order:
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
            raise RuntimeError(
                f"atlas overflow packing {key!r} ({w}x{h}): need >{max_h} height"
            )
        places[key] = (0, y)
        shelves.append([y, h2, w2])
        atlas_h = y + h2
    ah = max(4, (atlas_h + 3) & ~3)
    # Width: tight power-of-ish pad to 4 for GX, but keep ≤ max_w.
    used_w = max_w
    return places, used_w, min(max_h, ah)


def collect_unique_rects(
    data: bytes | bytearray, page_id: int
) -> dict[tuple[int, int, int, int], list[int]]:
    """Map source rect -> list of TPAG item file offsets."""
    _, tbase, _ = find_chunk(data, b"TPAG")
    count = u32(data, tbase)
    uniq: dict[tuple[int, int, int, int], list[int]] = {}
    for i in range(count):
        off = u32(data, tbase + 4 + i * 4)
        sx, sy, sw, sh = struct.unpack_from("<HHHH", data, off)
        page = struct.unpack_from("<h", data, off + 20)[0]
        if page != page_id or sw == 0 or sh == 0:
            continue
        key = (sx, sy, sw, sh)
        uniq.setdefault(key, []).append(off)
    return uniq


def _scale_for_rect(sw: int, sh: int, face_max: int, med_max: int, large_scale: int) -> int:
    """face_max < 0 means force 1:1 for everything."""
    if face_max < 0:
        return 1
    m = max(sw, sh)
    if m <= face_max:
        return 1
    if m <= med_max:
        return 2
    return max(2, large_scale)


def _build_pack(
    uniq: dict[tuple[int, int, int, int], list[int]],
    face_max: int,
    med_max: int,
    large_scale: int,
) -> tuple[
    list[tuple[tuple[int, int, int, int], int, int]],
    dict[tuple[int, int, int, int], int],
]:
    pack_items: list[tuple[tuple[int, int, int, int], int, int]] = []
    scales: dict[tuple[int, int, int, int], int] = {}
    for key in uniq:
        sx, sy, sw, sh = key
        scale = _scale_for_rect(sw, sh, face_max, med_max, large_scale)
        pw, ph = max(1, sw // scale), max(1, sh // scale)
        scales[key] = scale
        pack_items.append((key, pw, ph))
    return pack_items, scales


def repack_page(
    data: bytearray,
    orig: bytes,
    page_id: int,
    max_w: int,
    max_h: int,
) -> bytes | None:
    """Rebuild one TXTR page at 1:1 (WTL1 if tall). Patches TPAG UVs; returns new blob.

    Caller must install the blob via rewrite_txtr_blobs (supports slot growth).
    """
    otex = txtr_textures(orig)
    old_slot = otex[page_id]["size"]
    src_img = load_png_from_slot(orig, otex[page_id]["blob"], otex[page_id]["size"])
    uniq = collect_unique_rects(orig, page_id)
    if not uniq:
        print(f"page {page_id}: no TPAG rects, skip")
        return None

    # Prefer full 1:1 into a tall 1024×N atlas. Only degrade if shelf_pack cannot
    # fit under max_h — blob overflow is handled by rewrite_txtr_blobs growth.
    attempts: list[tuple[str, int, int, int, int]] = [
        # label, face_max (-1=all 1:1), med_max, large_scale, pad
        ("1:1 tall", -1, 0, 1, 1),
        ("1:1 tall pad0", -1, 0, 1, 0),
        ("face+med 1:1, big÷2", 128, 128, 2, 1),
        ("face+med 1:1, big÷2 pad0", 128, 128, 2, 0),
        ("face 1:1, med÷2, big÷4", 50, 128, 4, 1),
        ("face 1:1, med÷2, big÷4 pad0", 50, 128, 4, 0),
        ("face 1:1, else÷3", 50, 50, 3, 0),
    ]

    places = None
    pack_items: list[tuple[tuple[int, int, int, int], int, int]] = []
    scales: dict[tuple[int, int, int, int], int] = {}
    aw = ah = 0
    mode = ""
    last_err = ""
    png = b""

    for label, face_max, med_max, large_scale, pad in attempts:
        pack_items, scales = _build_pack(uniq, face_max, med_max, large_scale)
        try:
            places, aw, ah = shelf_pack(pack_items, max_w, max_h, pad=pad)
        except RuntimeError as e:
            last_err = str(e)
            places = None
            continue

        atlas = Image.new("RGBA", (aw, ah), (0, 0, 0, 0))
        for key, pw, ph in pack_items:
            sx, sy, sw, sh = key
            scale = scales[key]
            x, y = places[key]
            crop = src_img.crop((sx, sy, sx + sw, sy + sh))
            if scale > 1:
                crop = crop.resize((pw, ph), Image.Resampling.NEAREST)
            atlas.paste(crop, (x, y))

        # Tall 1:1 atlases must be WTL1 so the Wii never decodes 12–14MB of RGBA at once.
        if ah > 1024:
            blob_bytes = save_wtl1(atlas)
            enc = "WTL1"
        else:
            blob_bytes = save_png(atlas)
            enc = "PNG"

        png = blob_bytes
        mode = f"{label}/{enc}"
        break

    if places is None:
        raise SystemExit(f"page {page_id}: cannot pack: {last_err}")

    _, otbase, _ = find_chunk(orig, b"TPAG")
    _, ctbase, _ = find_chunk(data, b"TPAG")
    tcount = u32(orig, otbase)
    assert u32(data, ctbase) == tcount
    patched = face_n = large_n = 0
    new_uv = {}
    for key, pw, ph in pack_items:
        x, y = places[key]
        new_uv[key] = (x, y, pw, ph)

    for i in range(tcount):
        ooff = u32(orig, otbase + 4 + i * 4)
        coff = u32(data, ctbase + 4 + i * 4)
        sx, sy, sw, sh = struct.unpack_from("<HHHH", orig, ooff)
        page = struct.unpack_from("<h", orig, ooff + 20)[0]
        if page != page_id or sw == 0 or sh == 0:
            continue
        key = (sx, sy, sw, sh)
        nx, ny, nw, nh = new_uv[key]
        struct.pack_into("<HHHH", data, coff, nx, ny, nw, nh)
        patched += 1
        if scales[key] == 1:
            face_n += 1
        else:
            large_n += 1

    by_scale: dict[int, int] = {}
    for s in scales.values():
        by_scale[s] = by_scale.get(s, 0) + 1
    scale_txt = ", ".join(f"1:{s}={n}" for s, n in sorted(by_scale.items()))
    slices = (ah + 1023) // 1024
    grow = ""
    if len(png) > old_slot:
        grow = f", grow +{len(png) - old_slot}"
    print(
        f"page {page_id}: {src_img.size} -> {aw}x{ah} ({slices} GX slices), "
        f"mode={mode!r}, {len(uniq)} unique ({scale_txt}), "
        f"blob {len(png)}/{old_slot}{grow}, patched {patched} "
        f"(1:1 refs {face_n}, scaled refs {large_n})"
    )
    return png


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("input", type=Path, help="pristine data.win (usually data.win.orig)")
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument(
        "--base",
        type=Path,
        default=None,
        help="optional already-preprocessed data.win to keep other page edits; default=input",
    )
    ap.add_argument(
        "--pages",
        type=str,
        default="1,11,14,20,23,25",
        help="TXTR page ids to 1:1-repack as tall/WTL1 (comma-separated)",
    )
    ap.add_argument("--max-w", type=int, default=1024, help="GX texture width cap")
    ap.add_argument(
        "--max-h",
        type=int,
        default=8192,
        help="atlas height cap (renderer vertical-slices every 1024; WTL1 tiles amortize decode)",
    )
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    orig = args.input.read_bytes()
    base_path = args.base if args.base else args.input
    data = bytearray(base_path.read_bytes())
    pages = [int(x) for x in args.pages.split(",") if x.strip() != ""]

    otex = txtr_textures(orig)
    ctex = txtr_textures(data)
    for p in pages:
        ob, osz = otex[p]["blob"], otex[p]["size"]
        cb, csz = ctex[p]["blob"], ctex[p]["size"]
        if ob != cb or osz != csz:
            raise SystemExit(f"page {p} slot mismatch orig vs base")
        data[cb : cb + csz] = orig[ob : ob + osz]

    new_blobs: dict[int, bytes] = {}
    for p in pages:
        blob = repack_page(data, orig, p, args.max_w, args.max_h)
        if blob is not None:
            new_blobs[p] = blob

    if args.dry_run:
        need = sum(max(0, len(b) - otex[p]["size"]) for p, b in new_blobs.items())
        print(f"dry-run: no file written (would grow TXTR by ~{need} bytes if tight)")
        return 0

    delta = rewrite_txtr_blobs(data, new_blobs)
    if delta:
        print(f"TXTR/AUDO shift: {delta:+d} bytes (FORM now {len(data)})")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
