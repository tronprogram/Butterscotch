# Wii port — open todos

Status as of 2026-08-11: intro → early Ruins (Flowey dialog + Toriel battle) looks correct on Dolphin after texture residency / atlas work. Checkpoint committed.

## Confirmed working

- [x] Boot `data.win` from SD / Dolphin WiiSDSync
- [x] Native GX draw path (views, sprites, present)
- [x] Tall TXTR atlases via vertical GX slices (max dim 1024)
- [x] Opening intro scenes render (geometry / textures)
- [x] Felk Dolphin + `mcp-dolphin` debug loop (`tools/dolphin-mcp/`, `scripts/run-wii-dolphin-mcp.ps1`)
- [x] **Text drawing** — `gxDrawText` / `gxDrawTextColor` ported from GL legacy (glyphs + sprite fonts, kerning, align, corner colors)
- [x] **Fallen-child / final intro beat** — confirmed working after text path (2026-08-11)
- [x] **Audio** — external `*.ogg` + AESND streaming; intro music/SFX verified (2026-08-11)
- [x] **Title / menu interaction** — Wiimote→VK naming/title flow verified (2026-08-11)
- [x] **Dialog faces + battle sprites (TXTR 23/25)** — closed 2026-08-11
  - Offline: `scripts/build-wii-data-win.sh` ÷2 other width>1024 pages, then 1:1-repacks 23/25 into tall 1024×N atlases as **WTL1** (tiled PNGs)
  - Runtime: `gx_renderer` loads WTL1 one ≤1024-tall tile at a time (~4MB decode peak), RGB5A3 upload, LRU residency budget
  - Verified: Flowey portraits sharp; Toriel battle sprite full-res (not ÷2/÷3 crushed); no missing-face thrash on Dolphin

## Known gaps / next session

- [ ] **Other wide-page crunch** — pages 1/11/14/20 (etc.) still offline NN÷2 for GX width; mild chunkiness on some BG/FX. Optional: WTL1-style split or horizontal tiling at 1:1 if it bothers ship quality
- [ ] **Finale / heavy-room residency** — many atlases live at once may pressure the ~40MB RGB5A3 budget; watch eviction / missing sprites in True Pacifist ending and dense battles
- [x] **Frame hitch / stutter (movement + audio)** — merged from `wii/60hz-draw-experiment` 2026-08-11
  - Hybrid: 60Hz VI; audio first every VI; step + GML Draw every other VI (~30); odd VI = EFB→XFB duplicate
  - Saves: `run-wii-dolphin.sh` excludes `saves/` from rsync (no more LV20 barrier reseed)
  - Overlay: `stepMax` + `underrun`
  - Open: BGM pitch/tempo still slightly off on Dolphin; online 44.1→48k resample too heavy
- [x] **Saves not sticking / LV20 barrier default** — fixed 2026-08-11
  - Cause: debug `build-wii/.../saves/` (Chara LV20 room 236) + `rsync --delete` reseeding SD sync each launch
  - Fix: removed seeded files; `run-wii-dolphin.sh` excludes `saves/` from rsync
- [ ] **Wii system settings menu** — platform overlay (not Undertale's settings room): button layout, volume, soft reset, return to title/main menu
- [ ] **Saves verify** — confirm in-game save survives quit + relaunch via script
- [ ] **Strip debug / polish** — any leftover Wii logging, letterbox/teal edge artifact if still present
- [ ] **CI / packaging** — decide whether fork workflows should build `PLATFORM=wii`; HBC `apps/butterscotch` packaging docs; whether `build-wii-data-win.sh` should run in the default package path
- [ ] **Upstream PR hygiene** — do not open against `ButterscotchRunner/Butterscotch`.

## Debug references

- Felk Dolphin (Windows): `C:\Users\Tron\Downloads\dolphin-felk-scripting\Dolphin.exe`
- Bridge: `tools/dolphin-mcp/mcp_bridge.py` (port `55355`)
- Launch (Windows MCP): `scripts/run-wii-dolphin-mcp.ps1`
- Build (Windows): `scripts/build-wii.ps1` / `scripts/build-wii.sh`
- Build (macOS/Linux Docker): `scripts/build-wii-docker.sh` → `devkitpro/devkitppc` (`linux/arm64` on Apple Silicon)
- Texture `data.win` preprocess: `scripts/build-wii-data-win.sh` (needs `data.win.orig`; Pillow venv under `tools/.venv-wii-tex`)
- Run (mainline Dolphin): `scripts/run-wii-dolphin.sh` → `build-wii/apps/butterscotch/boot.dol`
- Stats overlay: HOME cycles on / profiler / off (FPS, MEM1/MEM2 used/free, TXTR pages, audio voices)
- Note: Felk Python-scripting + `mcp-dolphin` are Windows-oriented; no macOS Felk scripting build as of 2026-08.
