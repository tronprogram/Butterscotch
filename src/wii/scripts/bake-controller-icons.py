#!/usr/bin/env python3
"""Bake src/wii/assets/controller_icons.png into controller_icons_data.c

Edit the PNG (keep 256x256, ink on black), then re-run:
  python src/wii/scripts/bake-controller-icons.py

Reference layouts (artist truth):
  src/wii/assets/ref/wiimote-layout.png
  src/wii/assets/ref/gamecube-layout.png
  src/wii/assets/ref/classic-layout.png

Glyph conventions from those refs:
  Wiimote  — A/1/2/+/- in circles; B in a square (trigger); D-pad cross
             shared with Classic; direction prompts = D-pad with one arm lit.
  GameCube — different D-pad art (own GC_DPAD* set); large round A, smaller
             round B, capsule X/Y, capsule L/R/Z, small Start, C-stick.
  Classic  — same D-pad as Wiimote; lowercase a/b/x/y equal circles; L/R/ZL/ZR;
             +/-/HOME shared with Wiimote style.
"""
from __future__ import annotations

import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("Pillow required: pip install Pillow", file=sys.stderr)
    sys.exit(1)

ROOT = Path(__file__).resolve().parents[3]
ASSET = ROOT / "src" / "wii" / "assets" / "controller_icons.png"
OUT_C = ROOT / "src" / "wii" / "controller_icons_data.c"
OUT_H = ROOT / "src" / "wii" / "controller_icons.h"

ATLAS_W = 256
ATLAS_H = 256
CELL = 32
COLS = 8

# (id_name, col, row, short_label, note)
GLYPHS = [
    # Row 0 — Wiimote + Classic D-pad (same art) + shared +/-/HOME
    ("DPAD", 0, 0, "DPAD", "Wiimote/Classic cross"),
    ("DPAD_U", 1, 0, "DPAD_U", "WM/CL up arm lit"),
    ("DPAD_D", 2, 0, "DPAD_D", "WM/CL down arm lit"),
    ("DPAD_L", 3, 0, "DPAD_L", "WM/CL left arm lit"),
    ("DPAD_R", 4, 0, "DPAD_R", "WM/CL right arm lit"),
    ("PLUS", 5, 0, "PLUS", "Wiimote/Classic +"),
    ("MINUS", 6, 0, "MINUS", "Wiimote/Classic -"),
    ("HOME", 7, 0, "HOME", "house"),

    # Row 1 — pad silhouettes + Wiimote unique buttons
    ("WIIMOTE", 0, 1, "WIIMOTE", "remote body"),
    ("GAMECUBE", 1, 1, "GCN", "GC pad body"),
    ("CLASSIC", 2, 1, "CLASSIC", "classic body"),
    ("WM_A", 3, 1, "WM_A", "circle A"),
    ("WM_B", 4, 1, "WM_B", "square B trigger"),
    ("WM_1", 5, 1, "WM_1", "circle 1"),
    ("WM_2", 6, 1, "WM_2", "circle 2"),

    # Row 2 — GameCube D-pad family (different art from WM/CL) + misc
    ("GC_DPAD", 0, 2, "GC_DPAD", "GC d-pad cross"),
    ("GC_DPAD_U", 1, 2, "GC_DU", "GC up arm lit"),
    ("GC_DPAD_D", 2, 2, "GC_DD", "GC down arm lit"),
    ("GC_DPAD_L", 3, 2, "GC_DL", "GC left arm lit"),
    ("GC_DPAD_R", 4, 2, "GC_DR", "GC right arm lit"),
    ("GC_START", 5, 2, "GC_START", "small start"),
    ("GC_C", 6, 2, "GC_C", "C-stick"),

    # Row 3 — GameCube face / shoulders
    ("GC_A", 0, 3, "GC_A", "large circle A"),
    ("GC_B", 1, 3, "GC_B", "small circle B"),
    ("GC_X", 2, 3, "GC_X", "capsule X"),
    ("GC_Y", 3, 3, "GC_Y", "capsule Y"),
    ("GC_L", 4, 3, "GC_L", "shoulder L"),
    ("GC_R", 5, 3, "GC_R", "shoulder R"),
    ("GC_Z", 6, 3, "GC_Z", "capsule Z"),

    # Row 4 — Classic unique buttons (D-pad shared with Wiimote above)
    ("CL_A", 0, 4, "CL_a", "lowercase a"),
    ("CL_B", 1, 4, "CL_b", "lowercase b"),
    ("CL_X", 2, 4, "CL_x", "lowercase x"),
    ("CL_Y", 3, 4, "CL_y", "lowercase y"),
    ("CL_L", 4, 4, "CL_L", "shoulder L"),
    ("CL_R", 5, 4, "CL_R", "shoulder R"),
    ("CL_ZL", 6, 4, "CL_ZL", "ZL"),
    ("CL_ZR", 7, 4, "CL_ZR", "ZR"),
]


def fill_rect(px, x0, y0, x1, y1, v=40):
    for y in range(y0, y1):
        for x in range(x0, x1):
            if 0 <= x < ATLAS_W and 0 <= y < ATLAS_H:
                px[x, y] = max(px[x, y], v)


def fill_circle(px, cx, cy, r, v=55):
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            if (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r:
                if 0 <= x < ATLAS_W and 0 <= y < ATLAS_H:
                    px[x, y] = max(px[x, y], v)


def cell_border(px, ox, oy):
    for i in range(CELL):
        px[ox + i, oy] = 70
        px[ox + i, oy + CELL - 1] = 70
        px[ox, oy + i] = 70
        px[ox + CELL - 1, oy + i] = 70


def draw_dpad_shape(px, ox, oy, lit=None):
    """Shared D-pad cross. lit in {'U','D','L','R'} brightens that arm."""
    dim, bright = 50, 200
    # vertical bar
    v = bright if lit == "U" or lit == "D" else dim
    # draw whole cross dim first
    fill_rect(px, ox + 13, oy + 5, ox + 19, oy + 27, dim)
    fill_rect(px, ox + 5, oy + 13, ox + 27, oy + 19, dim)
    if lit == "U":
        fill_rect(px, ox + 13, oy + 5, ox + 19, oy + 14, bright)
    elif lit == "D":
        fill_rect(px, ox + 13, oy + 18, ox + 19, oy + 27, bright)
    elif lit == "L":
        fill_rect(px, ox + 5, oy + 13, ox + 14, oy + 19, bright)
    elif lit == "R":
        fill_rect(px, ox + 18, oy + 13, ox + 27, oy + 19, bright)


def draw_gc_dpad_shape(px, ox, oy, lit=None):
    """GameCube-style D-pad: chunkier cross + small arrow on lit arm (ref style)."""
    dim, bright = 55, 210
    # slightly thicker arms than WM/CL
    fill_rect(px, ox + 12, oy + 4, ox + 20, oy + 28, dim)
    fill_rect(px, ox + 4, oy + 12, ox + 28, oy + 20, dim)
    if lit == "U":
        fill_rect(px, ox + 12, oy + 4, ox + 20, oy + 14, bright)
        fill_rect(px, ox + 15, oy + 6, ox + 17, oy + 10, 255)
    elif lit == "D":
        fill_rect(px, ox + 12, oy + 18, ox + 20, oy + 28, bright)
        fill_rect(px, ox + 15, oy + 22, ox + 17, oy + 26, 255)
    elif lit == "L":
        fill_rect(px, ox + 4, oy + 12, ox + 14, oy + 20, bright)
        fill_rect(px, ox + 6, oy + 15, ox + 10, oy + 17, 255)
    elif lit == "R":
        fill_rect(px, ox + 18, oy + 12, ox + 28, oy + 20, bright)
        fill_rect(px, ox + 22, oy + 15, ox + 26, oy + 17, 255)


def draw_shape_hint(px, name, ox, oy):
    """Faint silhouette matching reference button shapes."""
    cx, cy = ox + CELL // 2, oy + CELL // 2
    if name == "DPAD":
        draw_dpad_shape(px, ox, oy, None)
    elif name == "DPAD_U":
        draw_dpad_shape(px, ox, oy, "U")
    elif name == "DPAD_D":
        draw_dpad_shape(px, ox, oy, "D")
    elif name == "DPAD_L":
        draw_dpad_shape(px, ox, oy, "L")
    elif name == "DPAD_R":
        draw_dpad_shape(px, ox, oy, "R")
    elif name == "GC_DPAD":
        draw_gc_dpad_shape(px, ox, oy, None)
    elif name == "GC_DPAD_U":
        draw_gc_dpad_shape(px, ox, oy, "U")
    elif name == "GC_DPAD_D":
        draw_gc_dpad_shape(px, ox, oy, "D")
    elif name == "GC_DPAD_L":
        draw_gc_dpad_shape(px, ox, oy, "L")
    elif name == "GC_DPAD_R":
        draw_gc_dpad_shape(px, ox, oy, "R")
    elif name in ("WM_A", "WM_1", "WM_2", "PLUS", "MINUS", "HOME"):
        fill_circle(px, cx, cy, 10, 50)
    elif name == "WM_B":
        fill_rect(px, ox + 8, oy + 8, ox + 24, oy + 24, 50)  # square trigger glyph
    elif name == "GC_A":
        fill_circle(px, cx, cy, 12, 55)  # large
    elif name == "GC_B":
        fill_circle(px, cx, cy, 7, 55)  # smaller
    elif name in ("GC_X", "GC_Y"):
        fill_rect(px, ox + 6, oy + 11, ox + 26, oy + 21, 55)  # capsule
    elif name in ("GC_L", "GC_R", "GC_Z"):
        fill_rect(px, ox + 5, oy + 10, ox + 27, oy + 22, 50)
    elif name == "GC_START":
        fill_circle(px, cx, cy, 5, 55)
    elif name == "GC_C":
        fill_circle(px, cx, cy, 8, 45)
        fill_circle(px, cx, cy, 3, 90)
    elif name.startswith("CL_") and len(name) == 4 and name[3] in "ABXY":
        fill_circle(px, cx, cy, 9, 50)
    elif name in ("CL_L", "CL_R", "CL_ZL", "CL_ZR"):
        fill_rect(px, ox + 5, oy + 10, ox + 27, oy + 22, 50)
    elif name == "WIIMOTE":
        fill_rect(px, ox + 12, oy + 4, ox + 20, oy + 28, 45)
    elif name == "GAMECUBE":
        fill_rect(px, ox + 5, oy + 8, ox + 27, oy + 24, 45)
    elif name == "CLASSIC":
        fill_rect(px, ox + 4, oy + 10, ox + 28, oy + 22, 45)
    else:
        fill_rect(px, ox + 8, oy + 8, ox + 24, oy + 24, 40)


def make_placeholder(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    img = Image.new("L", (ATLAS_W, ATLAS_H), 0)
    px = img.load()
    draw = ImageDraw.Draw(img)
    try:
        font = ImageFont.load_default(size=8)
    except TypeError:
        font = ImageFont.load_default()

    for name, col, row, label, _note in GLYPHS:
        ox, oy = col * CELL, row * CELL
        cell_border(px, ox, oy)
        draw_shape_hint(px, name, ox, oy)

        # Label for artist identification (may wrap)
        text = label
        if len(text) > 6:
            mid = (len(text) + 1) // 2
            lines = [text[:mid], text[mid:]]
        else:
            lines = [text]
        line_h = 7
        total_h = line_h * len(lines)
        ty0 = oy + max(1, (CELL - total_h) // 2)
        for li, line in enumerate(lines):
            bbox = draw.textbbox((0, 0), line, font=font)
            tw = bbox[2] - bbox[0]
            tx = ox + max(1, (CELL - tw) // 2)
            # darken under text for readability
            fill_rect(px, tx - 1, ty0 + li * line_h, tx + tw + 1, ty0 + li * line_h + 7, 20)
            draw.text((tx, ty0 + li * line_h), line, fill=255, font=font)

    img.save(path)
    print(f"wrote labeled placeholder {path} ({ATLAS_W}x{ATLAS_H})")
    print(f"{len(GLYPHS)} glyphs — paint over labels; keep cell shapes from refs:")
    print("  ref/wiimote-layout.png | ref/gamecube-layout.png | ref/classic-layout.png")
    for name, col, row, label, note in GLYPHS:
        print(f"  [{row},{col}] {name:10s}  label={label:8s}  ({note})")


def bake(path: Path) -> None:
    img = Image.open(path).convert("RGBA")
    if img.size != (ATLAS_W, ATLAS_H):
        raise SystemExit(f"{path} must be {ATLAS_W}x{ATLAS_H}, got {img.size}")
    # Flatten to RGBA byte stream (R,G,B,A per pixel).
    pixels = []
    for r, g, b, a in img.getdata():
        pixels.extend((r, g, b, a))

    enum_lines = []
    glyph_lines = []
    for i, (name, col, row, _label, _note) in enumerate(GLYPHS):
        enum_lines.append(f"    WII_CTRL_ICON_{name} = {i},")
        x, y = col * CELL, row * CELL
        glyph_lines.append(
            f"    {{ {x}, {y}, {CELL}, {CELL}, 0, 0, {CELL} }}, /* {name} */"
        )

    names = ", ".join(g[0] for g in GLYPHS)
    pix_count = ATLAS_W * ATLAS_H * 4
    h = f"""#ifndef _BS_CONTROLLER_ICONS_H_
#define _BS_CONTROLLER_ICONS_H_

#include <stdint.h>

/* Controller-icon atlas for Wii UI prompts (RGBA, colored).
 *
 * Rebuild from vendor packs:
 *   python src/wii/scripts/assemble-controller-icons.py
 *   python src/wii/scripts/bake-controller-icons.py
 *
 * Credits: src/wii/assets/CONTROLLER_ICONS_CREDITS.md
 * Grid: {CELL}x{CELL} cells, {COLS} columns. Full set: {names}
 */

#define WII_CTRL_ATLAS_W {ATLAS_W}
#define WII_CTRL_ATLAS_H {ATLAS_H}
#define WII_CTRL_CELL {CELL}
#define WII_CTRL_ICON_COUNT {len(GLYPHS)}

typedef enum {{
{chr(10).join(enum_lines)}
}} WiiCtrlIconId;

typedef struct {{
    uint16_t x, y;
    uint16_t w, h;
    int16_t xoffset;
    int16_t yoffset;
    int16_t xadvance;
}} WiiCtrlIconGlyph;

/* Packed RGBA8, {ATLAS_W}*{ATLAS_H}*4 bytes. */
extern const uint8_t wiiCtrlIconPixels[{pix_count}];
extern const WiiCtrlIconGlyph wiiCtrlIconGlyphs[WII_CTRL_ICON_COUNT];

void WiiCtrlIcons_init(void);
void WiiCtrlIcons_draw(WiiCtrlIconId id, float x, float y, float scale,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a);
float WiiCtrlIcons_advance(WiiCtrlIconId id, float scale);

#endif /* _BS_CONTROLLER_ICONS_H_ */
"""

    pix_lines = []
    for i in range(0, len(pixels), 16):
        chunk = pixels[i : i + 16]
        pix_lines.append("    " + ", ".join(f"0x{v:02x}" for v in chunk) + ",")

    c = f"""/* Auto-generated by bake-controller-icons.py — do not edit by hand.
 * Source atlas: src/wii/assets/controller_icons.png (RGBA)
 */
#include "controller_icons.h"

const uint8_t wiiCtrlIconPixels[{pix_count}] = {{
{chr(10).join(pix_lines)}
}};

const WiiCtrlIconGlyph wiiCtrlIconGlyphs[WII_CTRL_ICON_COUNT] = {{
{chr(10).join(glyph_lines)}
}};
"""

    OUT_H.write_text(h, encoding="utf-8", newline="\n")
    OUT_C.write_text(c, encoding="utf-8", newline="\n")
    print(f"wrote {OUT_H}")
    print(f"wrote {OUT_C} ({pix_count} RGBA bytes)")


def main() -> None:
    if not ASSET.exists() or "--force-placeholder" in sys.argv:
        make_placeholder(ASSET)
    bake(ASSET)


if __name__ == "__main__":
    main()
