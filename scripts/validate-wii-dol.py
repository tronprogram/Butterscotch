#!/usr/bin/env python3
"""Fail if a Wii DOL would be rejected by IPL/IOS / modern Dolphin."""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


def check(path: Path) -> list[str]:
    b = path.read_bytes()
    errs: list[str] = []
    if len(b) < 0x100:
        return ["file too small"]

    def u32(off: int) -> int:
        return struct.unpack_from(">I", b, off)[0]

    sections: list[tuple[str, int, int, int]] = []
    for i in range(7):
        size = u32(0x90 + i * 4)
        if size:
            sections.append((f"text[{i}]", u32(i * 4), u32(0x48 + i * 4), size))
    for i in range(11):
        size = u32(0xAC + i * 4)
        if size:
            sections.append((f"data[{i}]", u32(0x1C + i * 4), u32(0x64 + i * 4), size))

    bss_addr, bss_size = u32(0xD8), u32(0xDC)
    for name, off, addr, size in sections:
        if addr & 31:
            errs.append(f"{name} addr {addr:#x} not 32-byte aligned")
        if size & 31:
            errs.append(f"{name} size {size:#x} not 32-byte aligned")
        if off + size > len(b):
            errs.append(f"{name} extends past file end")

    # Overlap checks among load sections + bss
    ranges = [(n, a, a + s) for n, _, a, s in sections]
    if bss_size:
        ranges.append(("bss", bss_addr, bss_addr + bss_size))
    ranges.sort(key=lambda x: x[1])
    for (n0, a0, e0), (n1, a1, e1) in zip(ranges, ranges[1:]):
        if e0 > a1:
            errs.append(f"overlap {n0} [{a0:#x},{e0:#x}) vs {n1} [{a1:#x},{e1:#x})")
    return errs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dols", nargs="+", type=Path)
    args = ap.parse_args()
    failed = False
    for dol in args.dols:
        errs = check(dol)
        if errs:
            failed = True
            print(f"INVALID {dol}", file=sys.stderr)
            for e in errs:
                print(f"  - {e}", file=sys.stderr)
        else:
            print(f"OK {dol}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
