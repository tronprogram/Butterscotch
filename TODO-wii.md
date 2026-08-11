# Wii port — open todos

Status as of 2026-08-10: intro/story graphics mostly work after GX atlas slicing (1024×2048 → ≤1024-high slices). Leave the tree uncommitted until the next session unless noted.

## Confirmed working
- [x] Boot `data.win` from SD / Dolphin WiiSDSync
- [x] Native GX draw path (views, sprites, present)
- [x] Tall TXTR atlases via vertical GX slices (max dim 1024)
- [x] Opening intro scenes render (geometry / textures)
- [x] Felk Dolphin + `mcp-dolphin` debug loop (`tools/dolphin-mcp/`, `scripts/run-wii-dolphin-mcp.ps1`)

## Known gaps / next session
- [ ] **Text drawing** — `gxDrawText` / `gxDrawTextColor` are no-ops; intro narration and UI text missing
- [ ] **Fallen-child / final intro beat** — appears skipped; confirm room/event path vs missing draw/audio/wait
- [ ] **Audio** — AESND backend exists; verify music/SFX through full intro → title
- [ ] **Title / menu interaction** — Wiimote→VK works in code; exercise menus end-to-end
- [ ] **Saves** — Overlay FS + `saves/` path; verify write/read on SD sync
- [ ] **Strip debug / polish** — any leftover Wii logging, letterbox/teal edge artifact if still present
- [ ] **CI / packaging** — decide whether fork workflows should build `PLATFORM=wii`; HBC `apps/butterscotch` packaging docs
- [ ] **Upstream PR hygiene** — do not open against `ButterscotchRunner/Butterscotch` until text + intro parity are acceptable; keep work on `tronprogram/Butterscotch`

## Debug references
- Felk Dolphin: `C:\Users\Tron\Downloads\dolphin-felk-scripting\Dolphin.exe`
- Bridge: `tools/dolphin-mcp/mcp_bridge.py` (port `55355`)
- Launch: `scripts/run-wii-dolphin-mcp.ps1`
- Build: `scripts/build-wii.ps1` / `scripts/build-wii.sh` → `build-wii/apps/butterscotch/boot.dol`
