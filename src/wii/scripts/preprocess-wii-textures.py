#!/usr/bin/env python3
"""Offline-downscale wide TXTR pages in Undertale data.win for Wii GX.

GameMaker atlases wider than 1024 cannot upload to GX without a runtime
downscale (peak RAM ~16MB for 2048²). This tool bakes a nearest-neighbor
÷2 (or more) into data.win and scales TPAG source rects to match, so the
Wii loader treats them like normal ≤1024-wide pages.

Usage:
  tools/.venv-wii-tex/bin/python src/wii/scripts/preprocess-wii-textures.py \\
      build-wii/apps/butterscotch/data.win \\
      -o build-wii/apps/butterscotch/data.win
"""
from __future__ import annotations

import argparse
import io
import struct
import sys
from pathlib import Path

from PIL import Image


def u32(b: bytes, off: int) -> int:
    return struct.unpack_from("<I", b, off)[0]


def pack_u32(v: int) -> bytes:
    return struct.pack("<I", v)


def find_chunk(data: bytearray, tag: bytes) -> tuple[int, int, int]:
    """Return (header_pos, data_pos, data_size)."""
    pos = 8
    form_size = u32(data, 4)
    end = 8 + form_size
    while pos + 8 <= end:
        t = bytes(data[pos : pos + 4])
        size = u32(data, pos + 4)
        if t == tag:
            return pos, pos + 8, size
        pos += 8 + size
    raise SystemExit(f"chunk {tag!r} not found")


def parse_txtr(data: bytearray) -> tuple[int, int, int, list[dict]]:
    hdr, base, size = find_chunk(data, b"TXTR")
    count = u32(data, base)
    ptrs = [u32(data, base + 4 + i * 4) for i in range(count)]
    # Undertale 1.0 entries: scaled u32 + blobOffset u32 (8 bytes). Absolute offsets.
    textures: list[dict] = []
    for i, entry_off in enumerate(ptrs):
        scaled = u32(data, entry_off)
        blob_off = u32(data, entry_off + 4)
        if i + 1 < count:
            next_blob = u32(data, ptrs[i + 1] + 4)
            blob_size = next_blob - blob_off if next_blob > blob_off else 0
        else:
            # Last blob runs to end of TXTR chunk data
            blob_size = (base + size) - blob_off
        textures.append(
            {
                "index": i,
                "entry_off": entry_off,
                "scaled": scaled,
                "blob_off": blob_off,
                "blob_size": blob_size,
            }
        )
    return hdr, base, size, textures


def png_size(blob: bytes) -> tuple[int, int] | None:
    if len(blob) < 24 or blob[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    w, h = struct.unpack_from(">II", blob, 16)
    return int(w), int(h)


def downscale_png(blob: bytes, scale: int) -> bytes:
    img = Image.open(io.BytesIO(blob)).convert("RGBA")
    w, h = img.size
    nw, nh = max(1, w // scale), max(1, h // scale)
    out = img.resize((nw, nh), Image.Resampling.NEAREST)
    buf = io.BytesIO()
    out.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def scale_needed(w: int, h: int, max_dim: int = 1024) -> int:
    scale = 1
    while (w // scale) > max_dim:
        scale *= 2
    return scale


def patch_tpag_for_pages(data: bytearray, page_scales: dict[int, int]) -> int:
    if not page_scales:
        return 0
    _, base, size = find_chunk(data, b"TPAG")
    count = u32(data, base)
    patched = 0
    # Items are pointed to by absolute offsets in the pointer table.
    for i in range(count):
        item_off = u32(data, base + 4 + i * 4)
        # 10×u16 + i16 pageId
        page_id = struct.unpack_from("<h", data, item_off + 20)[0]
        scale = page_scales.get(page_id)
        if not scale or scale <= 1:
            continue
        sx, sy, sw, sh = struct.unpack_from("<HHHH", data, item_off)
        # target/bounding stay in sprite space — only atlas UVs shrink.
        nsx, nsy = sx // scale, sy // scale
        nsw, nsh = max(1, sw // scale) if sw else 0, max(1, sh // scale) if sh else 0
        struct.pack_into("<HHHH", data, item_off, nsx, nsy, nsw, nsh)
        patched += 1
    return patched


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="source data.win")
    ap.add_argument("-o", "--output", type=Path, required=True, help="output data.win")
    ap.add_argument(
        "--pages",
        type=str,
        default="",
        help="comma-separated page ids to process (default: all with width>1024 except --exclude)",
    )
    ap.add_argument(
        "--exclude",
        type=str,
        default="1,11,14,20,23,25",
        help="comma-separated page ids to leave full-res for WTL1 repack (default: all wide pages)",
    )
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    raw = bytearray(args.input.read_bytes())
    _, base, size, textures = parse_txtr(raw)
    force_pages: set[int] | None = None
    if args.pages.strip():
        force_pages = {int(x) for x in args.pages.split(",") if x.strip() != ""}
    exclude_pages = {int(x) for x in args.exclude.split(",") if x.strip() != ""}

    page_scales: dict[int, int] = {}
    replaced = 0
    saved = 0
    work = bytearray(raw)  # always edit a copy; dry-run discards it

    for tex in textures:
        i = tex["index"]
        blob_off = tex["blob_off"]
        blob_size = tex["blob_size"]
        if blob_off == 0 or blob_size <= 0:
            continue
        blob = bytes(work[blob_off : blob_off + blob_size])
        iend = blob.find(b"IEND")
        if iend < 4:
            continue
        end = iend + 8  # type(4)+crc(4); length field sits just before type
        png = blob[:end]
        wh = png_size(png)
        if not wh:
            continue
        w, h = wh
        if force_pages is not None and i not in force_pages:
            continue
        if i in exclude_pages:
            continue
        scale = scale_needed(w, h)
        if scale <= 1:
            continue

        new_png = downscale_png(png, scale)
        if len(new_png) > blob_size:
            print(
                f"ERROR page {i}: downscaled PNG {len(new_png)} > slot {blob_size}",
                file=sys.stderr,
            )
            return 1

        print(
            f"page {i}: {w}x{h} -> {w // scale}x{h // scale}  "
            f"png {len(png)} -> {len(new_png)} bytes (slot {blob_size})"
        )
        page_scales[i] = scale
        work[blob_off : blob_off + blob_size] = b"\x00" * blob_size
        work[blob_off : blob_off + len(new_png)] = new_png
        replaced += 1
        saved += len(png) - len(new_png)

    tpag_n = patch_tpag_for_pages(work, page_scales)
    print(f"downscaled {replaced} TXTR page(s), patched {tpag_n} TPAG item(s), saved ~{saved} png bytes")
    if args.dry_run:
        print("dry-run: no file written")
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(work)
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
