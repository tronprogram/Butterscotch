# Wii port — open todos

Status as of 2026-08-11: intro → early Ruins (Flowey dialog + Toriel battle) looks correct on Dolphin after texture residency / atlas work. Checkpoint committed.

## Confirmed working

- [x] Boot `data.win` from SD / Dolphin WiiSDSync
- [x] Native GX draw path (views, sprites, present)
- [x] Tall TXTR atlases via vertical GX slices (max dim 1024)
- [x] Opening intro scenes render (geometry / textures)
- [x] Felk Dolphin + `mcp-dolphin` debug loop (`tools/dolphin-mcp/`, `src/wii/scripts/run-wii-dolphin-mcp.ps1`)
- [x] **Text drawing** — `gxDrawText` / `gxDrawTextColor` ported from GL legacy (glyphs + sprite fonts, kerning, align, corner colors)
- [x] **Fallen-child / final intro beat** — confirmed working after text path (2026-08-11)
- [x] **Audio** — external `*.ogg` + AESND streaming; intro music/SFX verified (2026-08-11)
- [x] **Title / menu interaction** — Wiimote→VK naming/title flow verified (2026-08-11)
- [x] **Dialog faces + battle sprites (TXTR 23/25)** — closed 2026-08-11
  - Offline: `src/wii/scripts/build-wii-data-win.sh` 1:1-repacks wide pages into tall 1024×N **WTL1** (tiled PNGs); runtime loads one ≤1024-tall tile at a time
  - Verified: Flowey portraits sharp; Toriel battle sprite full-res; no missing-face thrash on Dolphin
- [x] **All remaining wide ÷2 pages → WTL1 1:1** — closed 2026-08-11
  - Pages **1 / 11 / 14 / 20 / 23 / 25** (every pristine width>1024 TXTR)
  - 1: Asriel hyperdeath / god-form; 11: Waterfall/Hotland props; 14: Omega Flowey; 20: JP fonts
  - Page 14 WTL1 denser than pristine PNG → `rewrite_txtr_blobs` grows TXTR and shifts AUDO ptrs

## Known gaps / next session

- [x] **Finale / heavy-room residency (Dolphin)** — verified 2026-08-11
  - Friends scene + True Pacifist boss (Asriel) stayed playable; no residency lag-out / missing-sprite thrash after full WTL1 1:1
  - Open: confirm same on real Wii hardware (Dolphin RAM behavior can be kinder)
- [x] **Frame hitch / stutter (movement + audio)** — merged from `wii/60hz-draw-experiment` 2026-08-11
  - Hybrid: 60Hz VI; audio first every VI; step + GML Draw every other VI (~30); odd VI = EFB→XFB duplicate
  - Saves: `run-wii-dolphin.sh` excludes `saves/` from rsync (no more LV20 barrier reseed)
  - Overlay: `stepMax` + `underrun`
  - Open: BGM pitch/tempo still slightly off on Dolphin; online 44.1→48k resample too heavy
- [x] **Saves not sticking / LV20 barrier default** — fixed 2026-08-11
  - Cause: debug `build-wii/.../saves/` (Chara LV20 room 236) + `rsync --delete` reseeding SD sync each launch
  - Fix: removed seeded files; `run-wii-dolphin.sh` excludes `saves/` from rsync
- [x] **Wii boot + system menu** — added 2026-08-11
  - Pre-`data.win` shell: Start / Options / Controls (rebind) / Extras (room jump) / About / Return to Wii Menu
  - In-game **HOME**: Resume / Options / Restart / Return to Wii Menu (`SYS_RETURNTOMENU`)
  - Persists `saves/wii_settings.json` (volume, overlay mode, mappings, start room)
- [ ] **Debug overlay quick-cycle** — HOME now opens system menu; overlay mode is under Options (Stats / Profiler / Off)
- [ ] **Saves verify** — confirm in-game save survives quit + relaunch via script
- [ ] **Strip debug / polish** — any leftover Wii logging, letterbox/teal edge artifact if still present
- [ ] **CI / packaging** — decide whether fork workflows should build `PLATFORM=wii`; HBC `apps/butterscotch` packaging docs; whether `build-wii-data-win.sh` should run in the default package path
- [ ] **Upstream PR hygiene** — do not open against `ButterscotchRunner/Butterscotch`.

## Debug references

- Felk Dolphin (Windows): `C:\Users\Tron\Downloads\dolphin-felk-scripting\Dolphin.exe`
- Bridge: `tools/dolphin-mcp/mcp_bridge.py` (port `55355`)
- Launch (Windows MCP): `src/wii/scripts/run-wii-dolphin-mcp.ps1`
- Build (Windows): `src/wii/scripts/build-wii.ps1` / `src/wii/scripts/build-wii.sh`
- Build (macOS/Linux Docker): `src/wii/scripts/build-wii-docker.sh` → `devkitpro/devkitppc` (`linux/arm64` on Apple Silicon)
- Texture `data.win` preprocess: `src/wii/scripts/build-wii-data-win.sh` (needs `data.win.orig`; Pillow venv under `tools/.venv-wii-tex`)
- Run (mainline Dolphin): `src/wii/scripts/run-wii-dolphin.sh` → `build-wii/apps/butterscotch/boot.dol`
- Stats overlay: Options → Debug Overlay (Stats / Profiler / Off). HOME opens system menu.
- Note: Felk Python-scripting + `mcp-dolphin` are Windows-oriented; no macOS Felk scripting build as of 2026-08.
