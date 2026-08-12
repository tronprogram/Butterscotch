#ifndef _BS_CONTROLLER_ICONS_H_
#define _BS_CONTROLLER_ICONS_H_

#include <stdint.h>

/* Controller-icon atlas for Wii UI prompts (RGBA, colored).
 *
 * Rebuild from vendor packs:
 *   python src/wii/scripts/assemble-controller-icons.py
 *   python src/wii/scripts/bake-controller-icons.py
 *
 * Credits: src/wii/assets/CONTROLLER_ICONS_CREDITS.md
 * Grid: 32x32 cells, 8 columns. Full set: DPAD, DPAD_U, DPAD_D, DPAD_L, DPAD_R, PLUS, MINUS, HOME, WIIMOTE, GAMECUBE, CLASSIC, WM_A, WM_B, WM_1, WM_2, GC_DPAD, GC_DPAD_U, GC_DPAD_D, GC_DPAD_L, GC_DPAD_R, GC_START, GC_C, GC_A, GC_B, GC_X, GC_Y, GC_L, GC_R, GC_Z, CL_A, CL_B, CL_X, CL_Y, CL_L, CL_R, CL_ZL, CL_ZR
 */

#define WII_CTRL_ATLAS_W 256
#define WII_CTRL_ATLAS_H 256
#define WII_CTRL_CELL 32
#define WII_CTRL_ICON_COUNT 37

typedef enum {
    WII_CTRL_ICON_DPAD = 0,
    WII_CTRL_ICON_DPAD_U = 1,
    WII_CTRL_ICON_DPAD_D = 2,
    WII_CTRL_ICON_DPAD_L = 3,
    WII_CTRL_ICON_DPAD_R = 4,
    WII_CTRL_ICON_PLUS = 5,
    WII_CTRL_ICON_MINUS = 6,
    WII_CTRL_ICON_HOME = 7,
    WII_CTRL_ICON_WIIMOTE = 8,
    WII_CTRL_ICON_GAMECUBE = 9,
    WII_CTRL_ICON_CLASSIC = 10,
    WII_CTRL_ICON_WM_A = 11,
    WII_CTRL_ICON_WM_B = 12,
    WII_CTRL_ICON_WM_1 = 13,
    WII_CTRL_ICON_WM_2 = 14,
    WII_CTRL_ICON_GC_DPAD = 15,
    WII_CTRL_ICON_GC_DPAD_U = 16,
    WII_CTRL_ICON_GC_DPAD_D = 17,
    WII_CTRL_ICON_GC_DPAD_L = 18,
    WII_CTRL_ICON_GC_DPAD_R = 19,
    WII_CTRL_ICON_GC_START = 20,
    WII_CTRL_ICON_GC_C = 21,
    WII_CTRL_ICON_GC_A = 22,
    WII_CTRL_ICON_GC_B = 23,
    WII_CTRL_ICON_GC_X = 24,
    WII_CTRL_ICON_GC_Y = 25,
    WII_CTRL_ICON_GC_L = 26,
    WII_CTRL_ICON_GC_R = 27,
    WII_CTRL_ICON_GC_Z = 28,
    WII_CTRL_ICON_CL_A = 29,
    WII_CTRL_ICON_CL_B = 30,
    WII_CTRL_ICON_CL_X = 31,
    WII_CTRL_ICON_CL_Y = 32,
    WII_CTRL_ICON_CL_L = 33,
    WII_CTRL_ICON_CL_R = 34,
    WII_CTRL_ICON_CL_ZL = 35,
    WII_CTRL_ICON_CL_ZR = 36,
} WiiCtrlIconId;

typedef struct {
    uint16_t x, y;
    uint16_t w, h;
    int16_t xoffset;
    int16_t yoffset;
    int16_t xadvance;
} WiiCtrlIconGlyph;

/* Packed RGBA8, 256*256*4 bytes. */
extern const uint8_t wiiCtrlIconPixels[262144];
extern const WiiCtrlIconGlyph wiiCtrlIconGlyphs[WII_CTRL_ICON_COUNT];

void WiiCtrlIcons_init(void);
void WiiCtrlIcons_draw(WiiCtrlIconId id, float x, float y, float scale,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a);
float WiiCtrlIcons_advance(WiiCtrlIconId id, float scale);

#endif /* _BS_CONTROLLER_ICONS_H_ */
