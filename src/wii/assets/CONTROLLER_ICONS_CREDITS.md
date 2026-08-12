# Controller icon atlas credits

`src/wii/assets/controller_icons.png` is assembled (RGBA) from:

## Openclipart Wii / Classic button SVG — public domain
- Author: CrazyTerabyte (denilsonsa)
- Source: https://openclipart.org/detail/230394/wii-buttons-mouse-buttons-keyboard-keys
- Used for: Wiimote A/B/1/2/+/-/HOME, shared D-pad, Classic a/b/x/y/L/R/ZL/ZR
- Modified: SVG symbols rendered to PNG and composited

## Kenney Input Prompts — CC0
- Author: Kenney
- Source: https://kenney.nl/assets/input-prompts
- Used for: pad body icons `WIIMOTE`, `CLASSIC`, `GAMECUBE`
- Modified: scaled into atlas cells

## GameCube Button Icons and Controls — CC BY 3.0
- Author: Zacksly
- Source: https://zacksly.itch.io/gamecube-button-icons-and-controls
- License: https://creativecommons.org/licenses/by/3.0/
- Used for: colored GameCube face/shoulder/D-pad/C-stick/Start glyphs
- Modified: scaled/composited into the atlas

## Rebuild
```
# Unpack Kenney + Zacksly under tools/controller-icons-src/; keep wii-buttons.svg
python src/wii/scripts/assemble-controller-icons.py
python src/wii/scripts/bake-controller-icons.py
```

Do not commit unpacked vendor trees under `tools/controller-icons-src/` (gitignored).
