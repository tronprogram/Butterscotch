# Wii port — open todos

Status as of 2026-08-12: playable through True Pacifist finale on Dolphin (friends scene + Asriel). Full-res WTL1 atlases, hybrid 60Hz present / ~30Hz step, boot + HOME system menu, controller icon glyphs + presets, HBC `icon.png`. Checkpoint: `429bef5` on `tronprogram/Butterscotch`.

## Confirmed working

- [x] Boot `data.win` from SD / Dolphin WiiSDSync
- [x] Native GX draw path (views, sprites, present)
- [x] Tall TXTR atlases via vertical GX slices (max dim 1024)
- [x] Opening intro → Ruins (Flowey dialog + Toriel battle)
- [x] Text drawing (`gxDrawText` / sprite fonts)
- [x] Audio — external `*.ogg` + AESND; Hold-ESC / `game_end` returns to Wii Menu cleanly
- [x] Title / menu interaction (Wiimote→VK)
- [x] **TXTR wide pages → WTL1 1:1** — pages 1 / 11 / 14 / 20 / 23 / 25
- [x] **Finale residency (Dolphin)** — friends scene + TP boss stay playable
- [x] **Frame hitch / stutter** — hybrid 60Hz present + ~30Hz step/draw
- [x] **BGM pitch / tempo** — fixed
- [x] **Saves** — in-game save survives quit + relaunch; launch script no longer reseeds `saves/`
- [x] **Boot + HOME system menu** — options, rebind, extras room jump, `SYS_RETURNTOMENU`
- [x] Felk Dolphin + `mcp-dolphin` debug loop (Windows)
- [x] Wii tooling under `src/wii/scripts/` (no root `scripts/*wii*` wrappers)

## Open

- [x] **CI** — `PLATFORM=wii` job in `.github/workflows/build.yml` (`devkitpro/devkitppc`, uploads `boot.dol`)
- [ ] **Real hardware smoke** — confirm finale residency + System Menu exit on a physical Wii (Dolphin can be kinder on RAM)
- [x] **Controller icon glyphs** — atlas (Openclipart PD + Kenney CC0 + Zacksly CC BY 3.0). Assemble/bake scripts + `CONTROLLER_ICONS_CREDITS.md`.
- [x] **Controller presets** — Vertical / Horizontal Wiimote, GameCube, Classic; Controls menu cycles presets + per-binding rebind; saved in `wii_settings.json`.
- [x] **HBC `icon.png`** — `packaging/wii/icon.png` staged by CMake POST_BUILD with `boot.dol` / `meta.xml`
  - [ ] Optional: richer logo / cover art; refresh `meta.xml` short/long description once naming/version settle
- [ ] **Channel WAD — plan (forwarder, not embed)** — System Menu channel that launches SD content
  - **Model:** tiny forwarder DOL → `sd:/apps/butterscotch/boot.dol`; **do not** embed `data.win` / oggs in the WAD
  - **Base WAD:** start with CustomizeMii **`StaticBase.wad`** (still banner/icon; no motion). Swap to **`WADder_Base_1/2/3`** (or other homebrew bases) later if we want zoom/rock/etc. — motion comes from the base’s brlan, not from StaticBase. Mirror: [Brawl345/customizemii/Base_WADs](https://github.com/Brawl345/customizemii/tree/master/Base_WADs). Prefer these / OHBC-built shells over ripped Nintendo channels.
  - [ ] Pick homebrew **title ID** + region; document install/uninstall notes
  - [ ] Banner kit: `banner.png` / `icon.png` / optional `sound.wav` sources + size checklist (IMET + U8 `banner.bin` / `icon.bin` / `sound.bin`)
  - [ ] Tooling path on Mac: **Sharpii-NetCore** (or CustomizeMii via Wine) — note exact commands once proven
  - [ ] Write `packaging/wii/CHANNEL.md` (or similar): StaticBase → inject forwarder + art; optional WADder upgrade path; legal note (no Nintendo banners / no pirated retail WADs)
  - [ ] Optional script later: “StaticBase (or chosen base) + forwarder DOL + our PNGs → emit Butterscotch channel WAD”
  - Out of scope for v1: NAND-installing the full game

## Standing rules (not tasks)

- Do **not** open PRs against `ButterscotchRunner/Butterscotch` — push only to `tronprogram/Butterscotch` (see `.cursor/rules/git-fork-destination.mdc`).
- HOME opens the system menu; debug overlay mode lives under Options (Stats / Profiler / Off). No separate HOME overlay-cycle.

## Debug references

- Build: `src/wii/scripts/build-wii-docker.sh` (macOS/Linux) · `src/wii/scripts/build-wii.ps1` (Windows)
- Run: `src/wii/scripts/run-wii-dolphin.sh` → stages `games/ut` on `.dolphin-user`
- Deltarune: `src/wii/scripts/run-wii-dolphin-deltarune.sh 1` → stages `games/dr` on the same SD (does not wipe ut)
- Convert: `src/wii/scripts/bs-convert.py ut|dr` (GUI: `bs-convert-gui.py`); GX needs a baked `data.win`
- Windows MCP: `src/wii/scripts/run-wii-dolphin-mcp.ps1` · bridge `tools/dolphin-mcp/mcp_bridge.py`
