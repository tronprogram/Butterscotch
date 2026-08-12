#!/usr/bin/env python3
"""Assemble colored controller_icons.png (RGBA) from vendor packs.

Sources under tools/controller-icons-src/:
  Openclipart Wii SVG  — Wiimote + Classic *button* glyphs (public domain)
  Kenney Input Prompts — pad body icons WIIMOTE / CLASSIC / GAMECUBE (CC0)
  Zacksly GC (CC BY)   — colored Soft Edge GameCube *button* glyphs

Then:
  python src/wii/scripts/bake-controller-icons.py
"""
from __future__ import annotations

import importlib.util
import re
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Pillow required", file=sys.stderr)
    sys.exit(1)

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "bake_ctrl", ROOT / "src" / "wii" / "scripts" / "bake-controller-icons.py"
)
bake = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(bake)

ATLAS_W, ATLAS_H, CELL = bake.ATLAS_W, bake.ATLAS_H, bake.CELL
GLYPHS, ASSET = bake.GLYPHS, bake.ASSET

SRC = ROOT / "tools" / "controller-icons-src"
ZACK = (
    SRC
    / "zacksly"
    / "GameCube Button Icons and Controls"
    / "Buttons Colored"
    / "Soft Edge"
    / "128w"
)
KENNEY_WII = SRC / "kenney" / "Nintendo Wii" / "Default"
KENNEY_GC = SRC / "kenney" / "Nintendo Gamecube" / "Default"
OCA_SVG = SRC / "wii-buttons.svg"
OCA_OUT = SRC / "openclipart-extracted"

OCA_SYMBOLS: dict[str, str] = {
    "DPAD": "dpad",
    "DPAD_U": "dpad_up",
    "DPAD_D": "dpad_down",
    "DPAD_L": "dpad_left",
    "DPAD_R": "dpad_right",
    "PLUS": "button_plus",
    "MINUS": "button_minus",
    "HOME": "button_home",
    "WM_A": "button_A",
    "WM_B": "button_B",
    "WM_1": "button_1",
    "WM_2": "button_2",
    "CL_A": "button_classic_a",
    "CL_B": "button_classic_b",
    "CL_X": "button_classic_x",
    "CL_Y": "button_classic_y",
    "CL_L": "button_classic_L",
    "CL_R": "button_classic_R",
    "CL_ZL": "button_classic_ZL",
    "CL_ZR": "button_classic_ZR",
}


def load_rgba(path: Path) -> Image.Image:
    return Image.open(path).convert("RGBA")


def paste_fit(dst: Image.Image, src: Image.Image, ox: int, oy: int, pad: int = 1) -> None:
    box = CELL - pad * 2
    im = src.copy()
    im.thumbnail((box, box), Image.Resampling.LANCZOS)
    px = ox + pad + (box - im.size[0]) // 2
    py = oy + pad + (box - im.size[1]) // 2
    dst.alpha_composite(im, (px, py))


def _symbol_viewbox(svg_text: str, sym_id: str) -> str:
    m = re.search(
        rf'<symbol\b[^>]*\bid="{re.escape(sym_id)}"[^>]*\bviewBox="([^"]+)"',
        svg_text,
        flags=re.I,
    )
    if not m:
        m = re.search(
            rf'<symbol\b[^>]*\bviewBox="([^"]+)"[^>]*\bid="{re.escape(sym_id)}"',
            svg_text,
            flags=re.I,
        )
    return m.group(1) if m else "0 0 34 34"


def extract_openclipart() -> dict[str, Path]:
    try:
        import cairosvg
    except ImportError:
        print("cairosvg required: pip install cairosvg", file=sys.stderr)
        sys.exit(1)

    if not OCA_SVG.exists():
        raise SystemExit(f"missing {OCA_SVG}")

    svg_text = OCA_SVG.read_text(encoding="utf-8")
    m = re.search(r"(<defs\b.*?</defs>)", svg_text, flags=re.I | re.S)
    if not m:
        raise SystemExit(f"no <defs> block in {OCA_SVG}")
    defs = m.group(1)

    OCA_OUT.mkdir(parents=True, exist_ok=True)
    out_paths: dict[str, Path] = {}
    print("Extracting Openclipart Wiimote + Classic button glyphs...")
    for glyph, sym in OCA_SYMBOLS.items():
        vb = _symbol_viewbox(svg_text, sym)
        wrapper = f"""<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     width="128" height="128" viewBox="{vb}">
{defs}
  <use xlink:href="#{sym}"/>
</svg>
"""
        png_path = OCA_OUT / f"{glyph}.png"
        png_bytes = cairosvg.svg2png(
            bytestring=wrapper.encode("utf-8"), output_width=128, output_height=128
        )
        png_path.write_bytes(png_bytes)
        out_paths[glyph] = png_path
        print(f"  {glyph:8s} <- #{sym}")
    return out_paths


def assemble() -> None:
    oca = extract_openclipart()

    MAP: dict[str, Path] = {
        **oca,
        "WIIMOTE": KENNEY_WII / "wii_controller.png",
        "CLASSIC": KENNEY_WII / "controller_wii_classic.png",
        "GAMECUBE": KENNEY_GC / "gamecube_controller.png",
        "GC_DPAD": ZACK / "D-Pad.png",
        "GC_DPAD_U": ZACK / "D-Pad Up.png",
        "GC_DPAD_D": ZACK / "D-Pad Down.png",
        "GC_DPAD_L": ZACK / "D-Pad Left.png",
        "GC_DPAD_R": ZACK / "D-Pad Right.png",
        "GC_START": ZACK / "Start Pause.png",
        "GC_C": ZACK / "C Stick.png",
        "GC_A": ZACK / "A.png",
        "GC_B": ZACK / "B.png",
        "GC_X": ZACK / "X.png",
        "GC_Y": ZACK / "Y.png",
        "GC_L": ZACK / "L Digital.png",
        "GC_R": ZACK / "R Digital.png",
        "GC_Z": ZACK / "Right Bumper.png",
    }

    missing = []
    atlas = Image.new("RGBA", (ATLAS_W, ATLAS_H), (0, 0, 0, 0))
    for name, col, row, _label, _note in GLYPHS:
        ox, oy = col * CELL, row * CELL
        path = MAP.get(name)
        if path is None or not path.exists():
            missing.append(f"{name} ({path})")
            continue
        paste_fit(atlas, load_rgba(path), ox, oy)

    ASSET.parent.mkdir(parents=True, exist_ok=True)
    # Keep alpha transparent — opaque black cells draw as black boxes over UI text.
    atlas.save(ASSET)
    print(f"wrote colored {ASSET}")
    if missing:
        print("MISSING:", ", ".join(missing))
        sys.exit(1)
    print(f"filled {len(GLYPHS)} glyphs (Kenney pad bodies)")


if __name__ == "__main__":
    assemble()
