#!/usr/bin/env python3
"""Convert a GameMaker data.win into Butterscotch Wii texture format.

Wii GX cannot sample 2048-wide atlases. Each game needs an offline bake:

  ut  Undertale — ÷2 leftover wide pages; face/battle pages as WTL1 (tiled PNG)
  dr  Deltarune — 2048 pages column-stacked as WTL2 (pre-swizzled RGB5A3)

The runner will not load an unconverted Deltarune data.win on hardware.

GUI: src/wii/scripts/bs-convert-gui.py
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT_DIR = Path(__file__).resolve().parent
VENV_PY = ROOT / "tools" / ".venv-wii-tex" / "bin" / "python"


def venv_python() -> Path:
    if VENV_PY.is_file():
        return VENV_PY
    return Path(sys.executable)


def convert_ut(src: Path, dst: Path) -> None:
    subprocess.check_call(
        ["bash", str(SCRIPT_DIR / "build-wii-data-win.sh"), str(src), str(dst)],
        cwd=str(ROOT),
    )


def convert_dr(src: Path, dst: Path) -> None:
    py = venv_python()
    subprocess.check_call(
        [str(py), str(SCRIPT_DIR / "repack-wii-gms2-wtl1.py"), str(src), "-o", str(dst)],
        cwd=str(ROOT),
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile", choices=("ut", "dr"), help="ut=Undertale, dr=Deltarune")
    ap.add_argument("-i", "--input", required=True, help="source data.win")
    ap.add_argument("-o", "--output", required=True, help="converted data.win")
    args = ap.parse_args()
    src = Path(args.input).expanduser().resolve()
    dst = Path(args.output).expanduser().resolve()
    if not src.is_file():
        print(f"missing input: {src}", file=sys.stderr)
        return 1
    dst.parent.mkdir(parents=True, exist_ok=True)
    print(f"convert {args.profile}: {src} -> {dst}")
    if args.profile == "ut":
        convert_ut(src, dst)
    else:
        convert_dr(src, dst)
    print(f"wrote {dst} ({dst.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
