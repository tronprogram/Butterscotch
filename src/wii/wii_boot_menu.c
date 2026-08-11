#include "wii_boot_menu.h"

#include "debug_font.h"
#include "runner.h"
#include "utils.h"
#include "wii_overlay.h"

#include <gccore.h>
#include <malloc.h>
#include <ogc/system.h>
#include <stdio.h>
#include <string.h>
#include <wiiuse/wpad.h>

#define UI_TEXT_SCALE 0.55f
#define UI_TITLE_SCALE 0.85f
#define UI_SUB_SCALE 0.45f
#define UI_LINE 22.0f
#define UI_TITLE_Y 48.0f
#define UI_CONTENT_PAD 18.0f

typedef enum {
    SCREEN_MAIN = 0,
    SCREEN_OPTIONS,
    SCREEN_CONTROLS,
    SCREEN_EXTRAS,
    SCREEN_ABOUT,
    SCREEN_REBIND,
} MenuScreen;

typedef struct {
    const char* label;
    const char* roomName; // NULL => clear override
} RoomPreset;

static const RoomPreset kRoomPresets[] = {
    { "Normal start", NULL },
    { "Intro story", "room_introstory" },
    { "Ruins flowerbed", "room_area1" },
    { "Snowdin town", "room_tundra_town" },
    { "Waterfall hub", "room_water_friendlyhub" },
    { "Hotland lab", "room_fire_lab1" },
    { "Core (pre-Mett)", "room_fire_core_premett" },
    { "Castle barrier", "room_castle_barrier" },
    { "True Lab hub", "room_truelab_hub" },
    { "Friends test", "room_friendstest" },
    { "Bring-it guys", "room_bringitinguys" },
    { "Asriel appears", "room_asrielappears" },
    { "Flowey X", "room_floweyx" },
    { "Battle room", "room_battle" },
};

typedef struct {
    bool fontReady;
    GXTexObj fontTex;
    uint8_t* fontTexData;
    MenuScreen screen;
    int cursor;
    int rebindIndex;
    bool waitingRebind;
    int roomPresetIndex;
} BootUi;

static BootUi gUi;

static void convertLinearRgba8ToTiled(uint8_t* dst, const uint8_t* src, int w, int h) {
    int tilesX = w / 4;
    int tilesY = h / 4;
    for (int ty = 0; ty < tilesY; ty++) {
        for (int tx = 0; tx < tilesX; tx++) {
            uint8_t* tile = dst + (ty * tilesX + tx) * 64;
            for (int row = 0; row < 4; row++) {
                for (int col = 0; col < 4; col++) {
                    int px = tx * 4 + col;
                    int py = ty * 4 + row;
                    const uint8_t* p = src + (py * w + px) * 4;
                    int i = row * 4 + col;
                    tile[i * 2 + 0] = p[3];
                    tile[i * 2 + 1] = p[0];
                    tile[32 + i * 2 + 0] = p[1];
                    tile[32 + i * 2 + 1] = p[2];
                }
            }
        }
    }
}

static const DebugFontGlyphEntry* lookupGlyph(uint8_t c) {
    if (c < DEBUGFONT_FIRST_CP || c > DEBUGFONT_LAST_CP) return NULL;
    return &debugFontGlyphs[c - DEBUGFONT_FIRST_CP];
}

static void uiInitFont(void) {
    if (gUi.fontReady) return;
    size_t linearBytes = (size_t)DEBUGFONT_ATLAS_W * (size_t)DEBUGFONT_ATLAS_H * 4;
    uint8_t* linear = (uint8_t*)safeMalloc(linearBytes);
    for (int i = 0; i < DEBUGFONT_ATLAS_W * DEBUGFONT_ATLAS_H; i++) {
        uint8_t a = debugFontPixels[i];
        linear[i * 4 + 0] = 255;
        linear[i * 4 + 1] = 255;
        linear[i * 4 + 2] = 255;
        linear[i * 4 + 3] = a;
    }
    size_t tiledBytes = linearBytes;
    gUi.fontTexData = (uint8_t*)memalign(32, tiledBytes);
    if (!gUi.fontTexData) {
        free(linear);
        return;
    }
    convertLinearRgba8ToTiled(gUi.fontTexData, linear, DEBUGFONT_ATLAS_W, DEBUGFONT_ATLAS_H);
    free(linear);
    DCFlushRange(gUi.fontTexData, tiledBytes);
    GX_InitTexObj(&gUi.fontTex, gUi.fontTexData, DEBUGFONT_ATLAS_W, DEBUGFONT_ATLAS_H,
                  GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&gUi.fontTex, GX_NEAR, GX_NEAR);
    gUi.fontReady = true;
}

static void uiSetupGx(int fbWidth, int fbHeight) {
    GX_SetViewport(0.0f, 0.0f, (f32)fbWidth, (f32)fbHeight, 0.0f, 1.0f);
    GX_SetScissor(0, 0, fbWidth, fbHeight);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

    Mtx44 proj;
    guOrtho(proj, 0.0f, (f32)fbHeight, 0.0f, (f32)fbWidth, 0.0f, 300.0f);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    Mtx mv;
    guMtxIdentity(mv);
    guMtxTransApply(mv, mv, 0.0f, 0.0f, -5.0f);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
}

static void uiFillRect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetNumChans(1);
    GX_SetNumTexGens(0);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position2f32(x, y);         GX_Color4u8(r, g, b, a);
    GX_Position2f32(x + w, y);     GX_Color4u8(r, g, b, a);
    GX_Position2f32(x + w, y + h); GX_Color4u8(r, g, b, a);
    GX_Position2f32(x, y + h);     GX_Color4u8(r, g, b, a);
    GX_End();
}

static void uiDrawText(float x, float y, float scale, uint8_t r, uint8_t g, uint8_t b, uint8_t a, const char* text) {
    if (!gUi.fontReady || !text) return;

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetNumChans(1);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_LoadTexObj(&gUi.fontTex, GX_TEXMAP0);

    float pen = x;
    float cursorY = y;
    int lineStart = 0;
    int len = (int)strlen(text);
    for (int i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            int lineLen = i - lineStart;
            pen = x;
            int quads = 0;
            for (int j = 0; j < lineLen; j++) {
                if (lookupGlyph((uint8_t)text[lineStart + j])) quads++;
            }
            if (quads > 0) {
                GX_Begin(GX_QUADS, GX_VTXFMT0, quads * 4);
                for (int j = 0; j < lineLen; j++) {
                    const DebugFontGlyphEntry* glyph = lookupGlyph((uint8_t)text[lineStart + j]);
                    if (!glyph) continue;
                    float qx0 = pen + (float)glyph->xoffset * scale;
                    float qy0 = cursorY + (float)glyph->yoffset * scale;
                    float qx1 = qx0 + (float)glyph->w * scale;
                    float qy1 = qy0 + (float)glyph->h * scale;
                    float u0 = (float)glyph->x / (float)DEBUGFONT_ATLAS_W;
                    float v0 = (float)glyph->y / (float)DEBUGFONT_ATLAS_H;
                    float u1 = ((float)glyph->x + (float)glyph->w) / (float)DEBUGFONT_ATLAS_W;
                    float v1 = ((float)glyph->y + (float)glyph->h) / (float)DEBUGFONT_ATLAS_H;
                    GX_Position2f32(qx0, qy0); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u0, v0);
                    GX_Position2f32(qx1, qy0); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u1, v0);
                    GX_Position2f32(qx1, qy1); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u1, v1);
                    GX_Position2f32(qx0, qy1); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u0, v1);
                    pen += (float)glyph->xadvance * scale;
                }
                GX_End();
            }
            cursorY += (float)DEBUGFONT_LINE_HEIGHT * scale * 0.80f;
            lineStart = i + 1;
        }
    }
}

static void uiPresent(GXRModeObj* rmode, void* xfb0, void* xfb1, u32* fbIndex) {
    void* xfb = (*fbIndex & 1u) ? xfb1 : xfb0;
    GX_DrawDone();
    GX_CopyDisp(xfb, GX_TRUE);
    GX_Flush();
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
    *fbIndex ^= 1u;
}

static const char* buttonName(uint32_t mask) {
    switch (mask) {
        case WPAD_BUTTON_LEFT:  return "D-Pad Left";
        case WPAD_BUTTON_RIGHT: return "D-Pad Right";
        case WPAD_BUTTON_UP:    return "D-Pad Up";
        case WPAD_BUTTON_DOWN:  return "D-Pad Down";
        case WPAD_BUTTON_A:     return "A";
        case WPAD_BUTTON_B:     return "B";
        case WPAD_BUTTON_PLUS:  return "+";
        case WPAD_BUTTON_MINUS: return "-";
        case WPAD_BUTTON_1:     return "1";
        case WPAD_BUTTON_2:     return "2";
        default: return "Button";
    }
}

static const char* gmlKeyName(int32_t key) {
    switch (key) {
        case VK_LEFT: return "Left";
        case VK_RIGHT: return "Right";
        case VK_UP: return "Up";
        case VK_DOWN: return "Down";
        case VK_ENTER: return "Enter";
        case VK_SHIFT: return "Shift";
        case VK_ESCAPE: return "Escape";
        case 'Z': return "Z (Confirm)";
        case 'X': return "X (Cancel)";
        case 'C': return "C (Menu)";
        default: return "Key";
    }
}

static const char* overlayName(int v) {
    switch (v) {
        case 0: return "Stats";
        case 1: return "Stats+Profiler";
        default: return "Off";
    }
}

static int findRoomPresetIndex(const WiiPortSettings* s) {
    if (!s->startRoomName[0]) return 0;
    for (int i = 0; i < (int)(sizeof(kRoomPresets) / sizeof(kRoomPresets[0])); i++) {
        if (kRoomPresets[i].roomName && strcmp(kRoomPresets[i].roomName, s->startRoomName) == 0) {
            return i;
        }
    }
    return 0;
}

static void applyRoomPreset(WiiPortSettings* s, int idx) {
    int n = (int)(sizeof(kRoomPresets) / sizeof(kRoomPresets[0]));
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    if (!kRoomPresets[idx].roomName) {
        s->startRoomName[0] = '\0';
    } else {
        strncpy(s->startRoomName, kRoomPresets[idx].roomName, sizeof(s->startRoomName) - 1);
        s->startRoomName[sizeof(s->startRoomName) - 1] = '\0';
    }
}

static float uiLineAdvance(float scale) {
    return (float)DEBUGFONT_LINE_HEIGHT * scale * 0.80f;
}

// Draws the panel chrome. Returns Y where screen content should start.
static float drawMenuChrome(int fbW, int fbH, const char* title, const char* subtitle, const char* hint) {
    uiFillRect(0, 0, (float)fbW, (float)fbH, 8, 8, 18, 255);
    uiFillRect(40, 36, (float)fbW - 80, (float)fbH - 72, 16, 16, 32, 230);
    uiFillRect(40, 36, (float)fbW - 80, 3, 255, 210, 90, 255);
    uiDrawText(56, UI_TITLE_Y, UI_TITLE_SCALE, 255, 230, 140, 255, title);

    float y = UI_TITLE_Y + uiLineAdvance(UI_TITLE_SCALE) + 6.0f;
    if (subtitle && subtitle[0]) {
        uiDrawText(56, y, UI_SUB_SCALE, 180, 180, 200, 255, subtitle);
        y += uiLineAdvance(UI_SUB_SCALE) + UI_CONTENT_PAD;
    } else {
        y += UI_CONTENT_PAD;
    }

    if (hint) {
        uiDrawText(56, (float)fbH - 52, 0.42f, 160, 160, 180, 255, hint);
    }
    return y;
}

static void drawSelectable(float x, float y, bool selected, const char* label) {
    if (selected) {
        uiFillRect(x - 8, y - 2, 520, UI_LINE + 2, 255, 210, 90, 40);
        uiDrawText(x, y, UI_TEXT_SCALE, 255, 230, 140, 255, label);
    } else {
        uiDrawText(x, y, UI_TEXT_SCALE, 220, 220, 230, 255, label);
    }
}

typedef enum {
    ACT_NONE = 0,
    ACT_START,
    ACT_EXIT_TO_WII_MENU,
    ACT_SOFT_RESET,
    ACT_RESUME,
} MenuAction;

static MenuAction tickScreen(
    WiiPortSettings* settings,
    const char* bundleDir,
    uint32_t down,
    bool inGame,
    bool* dirtySave
) {
    const int presetCount = (int)(sizeof(kRoomPresets) / sizeof(kRoomPresets[0]));

    if (gUi.waitingRebind) {
        // Capture first interesting button press as remapping.
        static const uint32_t capturable[] = {
            WPAD_BUTTON_LEFT, WPAD_BUTTON_RIGHT, WPAD_BUTTON_UP, WPAD_BUTTON_DOWN,
            WPAD_BUTTON_A, WPAD_BUTTON_B, WPAD_BUTTON_PLUS, WPAD_BUTTON_MINUS,
            WPAD_BUTTON_1, WPAD_BUTTON_2,
        };
        for (int i = 0; i < (int)(sizeof(capturable) / sizeof(capturable[0])); i++) {
            if (down & capturable[i]) {
                if (gUi.rebindIndex >= 0 && gUi.rebindIndex < settings->mapCount) {
                    // Keep GML key; change which Wii button triggers it.
                    // Avoid duplicate WPAD masks.
                    for (int j = 0; j < settings->mapCount; j++) {
                        if (j != gUi.rebindIndex && settings->maps[j].wpadButton == capturable[i]) {
                            settings->maps[j].wpadButton = settings->maps[gUi.rebindIndex].wpadButton;
                        }
                    }
                    settings->maps[gUi.rebindIndex].wpadButton = capturable[i];
                    *dirtySave = true;
                }
                gUi.waitingRebind = false;
                gUi.screen = SCREEN_CONTROLS;
                return ACT_NONE;
            }
        }
        if (down & WPAD_BUTTON_HOME) {
            gUi.waitingRebind = false;
            gUi.screen = SCREEN_CONTROLS;
        }
        return ACT_NONE;
    }

    if (down & WPAD_BUTTON_UP) {
        if (gUi.cursor > 0) gUi.cursor--;
    }
    if (down & WPAD_BUTTON_DOWN) {
        gUi.cursor++;
    }

    switch (gUi.screen) {
        case SCREEN_MAIN: {
            int items = inGame ? 4 : 6;
            if (gUi.cursor >= items) gUi.cursor = items - 1;
            if (down & WPAD_BUTTON_B) {
                if (inGame) return ACT_RESUME;
            }
            if (down & (WPAD_BUTTON_A | WPAD_BUTTON_PLUS)) {
                if (!inGame) {
                    switch (gUi.cursor) {
                        case 0: return ACT_START;
                        case 1: gUi.screen = SCREEN_OPTIONS; gUi.cursor = 0; break;
                        case 2: gUi.screen = SCREEN_CONTROLS; gUi.cursor = 0; break;
                        case 3: gUi.screen = SCREEN_EXTRAS; gUi.cursor = 0; break;
                        case 4: gUi.screen = SCREEN_ABOUT; gUi.cursor = 0; break;
                        case 5: return ACT_EXIT_TO_WII_MENU;
                    }
                } else {
                    switch (gUi.cursor) {
                        case 0: return ACT_RESUME;
                        case 1: gUi.screen = SCREEN_OPTIONS; gUi.cursor = 0; break;
                        case 2: return ACT_SOFT_RESET;
                        case 3: return ACT_EXIT_TO_WII_MENU;
                    }
                }
            }
            break;
        }
        case SCREEN_OPTIONS: {
            if (gUi.cursor > 2) gUi.cursor = 2;
            if (down & WPAD_BUTTON_B) {
                gUi.screen = SCREEN_MAIN;
                gUi.cursor = inGame ? 1 : 1;
            }
            if (gUi.cursor == 0) {
                if (down & WPAD_BUTTON_LEFT) {
                    settings->masterGain -= 0.05f;
                    if (settings->masterGain < 0.0f) settings->masterGain = 0.0f;
                    *dirtySave = true;
                }
                if (down & WPAD_BUTTON_RIGHT) {
                    settings->masterGain += 0.05f;
                    if (settings->masterGain > 1.0f) settings->masterGain = 1.0f;
                    *dirtySave = true;
                }
            } else if (gUi.cursor == 1) {
                if (down & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT | WPAD_BUTTON_A)) {
                    if (down & WPAD_BUTTON_LEFT) {
                        settings->debugOverlay = (settings->debugOverlay + 2) % 3;
                    } else {
                        settings->debugOverlay = (settings->debugOverlay + 1) % 3;
                    }
                    *dirtySave = true;
                }
            } else if (gUi.cursor == 2 && (down & (WPAD_BUTTON_A | WPAD_BUTTON_PLUS))) {
                gUi.screen = SCREEN_MAIN;
                gUi.cursor = inGame ? 1 : 1;
            }
            break;
        }
        case SCREEN_CONTROLS: {
            int items = settings->mapCount + 2; // maps + reset + back
            if (gUi.cursor >= items) gUi.cursor = items - 1;
            if (down & WPAD_BUTTON_B) {
                gUi.screen = SCREEN_MAIN;
                gUi.cursor = inGame ? 0 : 2;
            }
            if (down & (WPAD_BUTTON_A | WPAD_BUTTON_PLUS)) {
                if (gUi.cursor < settings->mapCount) {
                    gUi.rebindIndex = gUi.cursor;
                    gUi.waitingRebind = true;
                    gUi.screen = SCREEN_REBIND;
                } else if (gUi.cursor == settings->mapCount) {
                    WiiSettings_installDefaultMaps(settings);
                    *dirtySave = true;
                } else {
                    gUi.screen = SCREEN_MAIN;
                    gUi.cursor = inGame ? 0 : 2;
                }
            }
            break;
        }
        case SCREEN_EXTRAS: {
            if (gUi.cursor > 2) gUi.cursor = 2;
            if (down & WPAD_BUTTON_B) {
                gUi.screen = SCREEN_MAIN;
                gUi.cursor = 3;
            }
            if (gUi.cursor == 0) {
                if (down & WPAD_BUTTON_LEFT) {
                    gUi.roomPresetIndex = (gUi.roomPresetIndex + presetCount - 1) % presetCount;
                    applyRoomPreset(settings, gUi.roomPresetIndex);
                    *dirtySave = true;
                }
                if (down & WPAD_BUTTON_RIGHT) {
                    gUi.roomPresetIndex = (gUi.roomPresetIndex + 1) % presetCount;
                    applyRoomPreset(settings, gUi.roomPresetIndex);
                    *dirtySave = true;
                }
            } else if (gUi.cursor == 1 && (down & (WPAD_BUTTON_A | WPAD_BUTTON_PLUS))) {
                WiiSettings_setDefaults(settings);
                gUi.roomPresetIndex = 0;
                *dirtySave = true;
                WiiSettings_save(settings, bundleDir);
            } else if (gUi.cursor == 2 && (down & (WPAD_BUTTON_A | WPAD_BUTTON_PLUS))) {
                gUi.screen = SCREEN_MAIN;
                gUi.cursor = 3;
            }
            break;
        }
        case SCREEN_ABOUT: {
            gUi.cursor = 0;
            if (down & (WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_PLUS)) {
                gUi.screen = SCREEN_MAIN;
                gUi.cursor = inGame ? 0 : 4;
            }
            break;
        }
        case SCREEN_REBIND:
            break;
    }
    return ACT_NONE;
}

static void drawScreen(int fbW, int fbH, const WiiPortSettings* settings, bool inGame) {
    char line[160];
    float x = 64.0f;
    float y;

    if (gUi.waitingRebind || gUi.screen == SCREEN_REBIND) {
        y = drawMenuChrome(fbW, fbH, "Rebind", NULL, "Press a Wiimote button  |  HOME cancel");
        if (gUi.rebindIndex >= 0 && gUi.rebindIndex < settings->mapCount) {
            snprintf(line, sizeof(line), "Remapping: %s", gmlKeyName(settings->maps[gUi.rebindIndex].gmlKey));
            uiDrawText(x, y, UI_TEXT_SCALE, 255, 255, 255, 255, line);
            uiDrawText(x, y + UI_LINE * 2, UI_TEXT_SCALE, 200, 200, 210, 255, "Waiting for button...");
        }
        return;
    }

    switch (gUi.screen) {
        case SCREEN_MAIN:
            y = drawMenuChrome(fbW, fbH,
                               inGame ? "Butterscotch  |  System" : "Butterscotch",
                               inGame ? "Game paused" : "Wii Undertale port",
                               "D-Pad move  |  A confirm  |  B back");
            if (!inGame) {
                drawSelectable(x, y + UI_LINE * 0, gUi.cursor == 0, "Start Game");
                drawSelectable(x, y + UI_LINE * 1, gUi.cursor == 1, "Options");
                drawSelectable(x, y + UI_LINE * 2, gUi.cursor == 2, "Controls");
                drawSelectable(x, y + UI_LINE * 3, gUi.cursor == 3, "Extras");
                drawSelectable(x, y + UI_LINE * 4, gUi.cursor == 4, "About");
                drawSelectable(x, y + UI_LINE * 5, gUi.cursor == 5, "Return to Wii Menu");
            } else {
                drawSelectable(x, y + UI_LINE * 0, gUi.cursor == 0, "Resume");
                drawSelectable(x, y + UI_LINE * 1, gUi.cursor == 1, "Options");
                drawSelectable(x, y + UI_LINE * 2, gUi.cursor == 2, "Restart Game");
                drawSelectable(x, y + UI_LINE * 3, gUi.cursor == 3, "Return to Wii Menu");
            }
            break;
        case SCREEN_OPTIONS:
            y = drawMenuChrome(fbW, fbH, "Options", NULL, "Left/Right adjust  |  B back");
            snprintf(line, sizeof(line), "Master Volume ...... %3d%%", (int)(settings->masterGain * 100.0f + 0.5f));
            drawSelectable(x, y + UI_LINE * 0, gUi.cursor == 0, line);
            snprintf(line, sizeof(line), "Debug Overlay ...... %s", overlayName(settings->debugOverlay));
            drawSelectable(x, y + UI_LINE * 1, gUi.cursor == 1, line);
            drawSelectable(x, y + UI_LINE * 2, gUi.cursor == 2, "Back");
            break;
        case SCREEN_CONTROLS:
            y = drawMenuChrome(fbW, fbH, "Controls", NULL, "A rebind  |  B back");
            for (int i = 0; i < settings->mapCount; i++) {
                snprintf(line, sizeof(line), "%-12s -> %s",
                         buttonName(settings->maps[i].wpadButton),
                         gmlKeyName(settings->maps[i].gmlKey));
                drawSelectable(x, y + UI_LINE * (float)i, gUi.cursor == i, line);
            }
            drawSelectable(x, y + UI_LINE * (float)settings->mapCount, gUi.cursor == settings->mapCount, "Reset to defaults");
            drawSelectable(x, y + UI_LINE * (float)(settings->mapCount + 1), gUi.cursor == settings->mapCount + 1, "Back");
            break;
        case SCREEN_EXTRAS:
            y = drawMenuChrome(fbW, fbH, "Extras", NULL, "Left/Right change room  |  B back");
            snprintf(line, sizeof(line), "Start Room  < %s >", kRoomPresets[gUi.roomPresetIndex].label);
            drawSelectable(x, y + UI_LINE * 0, gUi.cursor == 0, line);
            if (settings->startRoomName[0]) {
                snprintf(line, sizeof(line), "  (%s)", settings->startRoomName);
                uiDrawText(x, y + UI_LINE * 0.85f, 0.40f, 160, 170, 190, 255, line);
            }
            drawSelectable(x, y + UI_LINE * 2, gUi.cursor == 1, "Reset all port settings");
            drawSelectable(x, y + UI_LINE * 3, gUi.cursor == 2, "Back");
            uiDrawText(x, y + UI_LINE * 5, 0.40f, 170, 150, 150, 255,
                       "Room jump skips normal boot flow — saves may look weird.");
            break;
        case SCREEN_ABOUT:
            y = drawMenuChrome(fbW, fbH, "About", NULL, "A / B back");
            uiDrawText(x, y, UI_TEXT_SCALE, 230, 230, 240, 255, "Butterscotch on Wii");
            uiDrawText(x, y + UI_LINE * 1.2f, 0.45f, 200, 200, 210, 255,
                       "GameMaker runner port — GX renderer, AESND audio,");
            uiDrawText(x, y + UI_LINE * 2.2f, 0.45f, 200, 200, 210, 255,
                       "WTL1 tiled atlases for full-res sprites.");
            uiDrawText(x, y + UI_LINE * 3.5f, 0.45f, 200, 200, 210, 255,
                       "Fork playground. Not affiliated with Toby Fox.");
            uiDrawText(x, y + UI_LINE * 5, 0.45f, 255, 210, 90, 255,
                       "HOME in-game opens this system menu.");
            drawSelectable(x, y + UI_LINE * 7, true, "Back");
            break;
        default:
            break;
    }
}

static MenuAction runMenuLoop(
    GXRModeObj* rmode,
    void* xfb0,
    void* xfb1,
    u32* fbIndex,
    const char* bundleDir,
    WiiPortSettings* settings,
    bool inGame,
    Runner* runner
) {
    (void)runner;
    uiInitFont();
    gUi.screen = SCREEN_MAIN;
    gUi.cursor = 0;
    gUi.waitingRebind = false;
    gUi.roomPresetIndex = findRoomPresetIndex(settings);
    bool dirty = false;
    MenuAction result = ACT_NONE;

    int fbW = (int)rmode->fbWidth;
    int fbH = (int)rmode->efbHeight;

    while (result == ACT_NONE) {
        WPAD_ScanPads();
        uint32_t down = WPAD_ButtonsDown(0);

        // HOME resumes when already in the in-game system menu.
        if (inGame && (down & WPAD_BUTTON_HOME) && gUi.screen == SCREEN_MAIN && !gUi.waitingRebind) {
            result = ACT_RESUME;
            break;
        }

        result = tickScreen(settings, bundleDir, down, inGame, &dirty);

        GXColor clear = { 8, 8, 18, 255 };
        GX_SetCopyClear(clear, GX_MAX_Z24);
        uiSetupGx(fbW, fbH);
        // Force a clear by copying once; draw on top.
        uiFillRect(0, 0, (float)fbW, (float)fbH, 8, 8, 18, 255);
        drawScreen(fbW, fbH, settings, inGame);
        uiPresent(rmode, xfb0, xfb1, fbIndex);
    }

    if (dirty) {
        WiiSettings_save(settings, bundleDir);
    }
    return result;
}

WiiBootResult WiiBootMenu_run(
    GXRModeObj* rmode,
    void* xfb0,
    void* xfb1,
    u32* fbIndex,
    const char* bundleDir,
    WiiPortSettings* settings
) {
    WPAD_Init();
    logInfo("WiiBootMenu: entering pre-boot shell\n");
    MenuAction act = runMenuLoop(rmode, xfb0, xfb1, fbIndex, bundleDir, settings, false, NULL);
    if (act == ACT_EXIT_TO_WII_MENU) return WII_BOOT_RETURN_TO_MENU;
    return WII_BOOT_START_GAME;
}

bool WiiSystemMenu_run(
    GXRModeObj* rmode,
    void* xfb0,
    void* xfb1,
    u32* fbIndex,
    const char* bundleDir,
    WiiPortSettings* settings,
    Runner* runner,
    bool* exitToLoader
) {
    if (exitToLoader) *exitToLoader = false;
    MenuAction act = runMenuLoop(rmode, xfb0, xfb1, fbIndex, bundleDir, settings, true, runner);
    if (act == ACT_EXIT_TO_WII_MENU) {
        if (exitToLoader) *exitToLoader = true;
        return false;
    }
    if (act == ACT_SOFT_RESET) return true;
    return false;
}
