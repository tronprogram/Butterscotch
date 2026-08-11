#!/usr/bin/env python3
"""Pad Wii DOL text/data section sizes to 32-byte multiples.

Modern Dolphin (DolReader) rejects DOLs whose section address/size are not
32-byte aligned and fails boot with "Failed to init core".
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

ALIGN = 32


def align_up(n: int, a: int = ALIGN) -> int:
    return (n + a - 1) & ~(a - 1)


def pad_dol(path: Path, dry_run: bool = False) -> bool:
    raw = bytearray(path.read_bytes())
    if len(raw) < 0x100:
        raise SystemExit(f"DOL too small: {path}")

    def u32(off: int) -> int:
        return struct.unpack_from(">I", raw, off)[0]

    sections: list[list] = []
    for i in range(7):
        size = u32(0x90 + i * 4)
        if size:
            sections.append([i * 4, 0x90 + i * 4, u32(i * 4), u32(0x48 + i * 4), size, "text", i])
    for i in range(11):
        size = u32(0xAC + i * 4)
        if size:
            sections.append(
                [0x1C + i * 4, 0xAC + i * 4, u32(0x1C + i * 4), u32(0x64 + i * 4), size, "data", i]
            )

    if not any(s[4] % ALIGN for s in sections):
        print(f"already aligned: {path}")
        return False

    sections.sort(key=lambda s: s[2])
    new_blob = bytearray(raw[:0x100])
    max_end = 0
    for hdr_off_field, hdr_size_field, old_off, load_addr, size, kind, idx in sections:
        data = raw[old_off : old_off + size]
        new_size = align_up(size)
        if new_size != size:
            data = data + b"\x00" * (new_size - size)
            print(f"pad {kind}[{idx}] {size:#x} -> {new_size:#x}")
        new_off = len(new_blob)
        new_blob.extend(data)
        struct.pack_into(">I", new_blob, hdr_off_field, new_off)
        struct.pack_into(">I", new_blob, hdr_size_field, new_size)
        max_end = max(max_end, load_addr + new_size)

    bss_addr = u32(0xD8)
    bss_size = u32(0xDC)
    if bss_size and max_end > bss_addr:
        delta = max_end - bss_addr
        new_bss_addr = max_end
        new_bss_size = bss_size - delta if bss_size > delta else 0
        print(f"bump bss {bss_addr:#x}+{bss_size:#x} -> {new_bss_addr:#x}+{new_bss_size:#x}")
        struct.pack_into(">I", new_blob, 0xD8, new_bss_addr)
        struct.pack_into(">I", new_blob, 0xDC, new_bss_size)

    if dry_run:
        print(f"dry-run {path}: {len(raw)} -> {len(new_blob)} bytes")
        return True

    path.write_bytes(new_blob)
    print(f"wrote {path}: {len(raw)} -> {len(new_blob)} bytes")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("dols", nargs="+", type=Path)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    for dol in args.dols:
        pad_dol(dol, dry_run=args.dry_run)
    return 0


if __name__ == "__main__":
    sys.exit(main())
