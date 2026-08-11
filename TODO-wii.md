# Wii port — open todos

Status as of 2026-08-11: playable through True Pacifist finale on Dolphin (friends scene + Asriel). Full-res WTL1 atlases, hybrid 60Hz present / ~30Hz step, boot + HOME system menu. Checkpoint: `26f0319` on `tronprogram/Butterscotch`.

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

## Standing rules (not tasks)

- Do **not** open PRs against `ButterscotchRunner/Butterscotch` — push only to `tronprogram/Butterscotch` (see `.cursor/rules/git-fork-destination.mdc`).
- HOME opens the system menu; debug overlay mode lives under Options (Stats / Profiler / Off). No separate HOME overlay-cycle.

## Debug references

- Build: `src/wii/scripts/build-wii-docker.sh` (macOS/Linux) · `src/wii/scripts/build-wii.ps1` (Windows)
- Run: `src/wii/scripts/run-wii-dolphin.sh` → `build-wii/apps/butterscotch/boot.dol`
- Textures: `src/wii/scripts/build-wii-data-win.sh` (needs `data.win.orig`; Pillow venv `tools/.venv-wii-tex`)
- Windows MCP: `src/wii/scripts/run-wii-dolphin-mcp.ps1` · bridge `tools/dolphin-mcp/mcp_bridge.py`
