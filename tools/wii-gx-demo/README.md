# Wii GX graphics demo

Standalone Dolphin/Wii app that exercises the same GX techniques Butterscotch uses
**without** `data.win`, the Runner, or GML.

Use this to decide whether slowdowns / missing draws are:

- **GX implementation issues** (atlas slicing, downscale, UV split, present), or
- **engine integration issues** (Runner draw order, TPAG data, memory pressure from game systems)

## What it tests

| Feature | How |
|---|---|
| 1024×1024 atlas | Solid checkerboard sprites |
| 1024×2048 tall atlas | Vertical GX slices (max dim 1024) |
| 2048×2048 wide atlas | Nearest-neighbor ½ downscale → 1024×1024 |
| Cross-slice sprites | Quads that span Y=1024, split + bilerp |
| `drawSprite` | Batch of moving sprites |
| `drawSpritePart` | Sub-rect samples |
| `drawSpriteTiled` | Scrolling tiled ground |
| Camera scroll | Subpixel view origin |
| Blend | Alpha sprites over background |
| Present | EFB→XFB + VSync |

## Controls (Wiimote)

- **D-pad L/R** — sprite count preset (16 / 64 / 256 / 1024)
- **D-pad U/D** — scroll speed
- **A** — toggle tall-atlas / wide-atlas stress
- **B** — toggle tiled background
- **1** — toggle cross-slice test quads
- **HOME** — quit

## Build / run

```bash
./src/wii/scripts/build-wii-gx-demo-docker.sh
./src/wii/scripts/run-wii-gx-demo.sh
```

Output DOL: `build-wii-gx-demo/boot.dol`
