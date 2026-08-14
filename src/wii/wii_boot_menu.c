#include "wii_boot_menu.h"

#include "controller_icons.h"
#include "debug_font.h"
#include "runner.h"
#include "utils.h"
#include "wii_games.h"
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
    SCREEN_GAMES,
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
    const WiiGameEntry* games;
    int gameCount;
    int gameIndex;
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

    size_t tiledBytes = (size_t)DEBUGFONT_ATLAS_W * (size_t)DEBUGFONT_ATLAS_H * 4;
    gUi.fontTexData = (uint8_t*)memalign(32, tiledBytes);
    requireNotNull(gUi.fontTexData);
    convertLinearRgba8ToTiled(gUi.fontTexData, linear, DEBUGFONT_ATLAS_W, DEBUGFONT_ATLAS_H);
    free(linear);
    DCFlushRange(gUi.fontTexData, tiledBytes);

    GX_InitTexObj(&gUi.fontTex, gUi.fontTexData, DEBUGFONT_ATLAS_W, DEBUGFONT_ATLAS_H,
                  GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&gUi.fontTex, GX_NEAR, GX_NEAR);
    GX_InvalidateTexAll();
    gUi.fontReady = true;

    WiiCtrlIcons_init();
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
    if (WII_INPUT_IS_GC(mask)) {
        switch (WII_INPUT_GC_PAD(mask)) {
            case PAD_BUTTON_LEFT:  return "GC Left";
            case PAD_BUTTON_RIGHT: return "GC Right";
            case PAD_BUTTON_UP:    return "GC Up";
            case PAD_BUTTON_DOWN:  return "GC Down";
            case PAD_BUTTON_A:     return "GC A";
            case PAD_BUTTON_B:     return "GC B";
            case PAD_BUTTON_X:     return "GC X";
            case PAD_BUTTON_Y:     return "GC Y";
            case PAD_BUTTON_START: return "GC Start";
            case PAD_TRIGGER_Z:    return "GC Z";
            case PAD_TRIGGER_L:    return "GC L";
            case PAD_TRIGGER_R:    return "GC R";
            default:               return "GC Button";
        }
    }
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
        case WPAD_BUTTON_HOME:  return "Home";
        case WPAD_CLASSIC_BUTTON_LEFT:  return "CL Left";
        case WPAD_CLASSIC_BUTTON_RIGHT: return "CL Right";
        case WPAD_CLASSIC_BUTTON_UP:    return "CL Up";
        case WPAD_CLASSIC_BUTTON_DOWN:  return "CL Down";
        case WPAD_CLASSIC_BUTTON_A:     return "CL a";
        case WPAD_CLASSIC_BUTTON_B:     return "CL b";
        case WPAD_CLASSIC_BUTTON_X:     return "CL x";
        case WPAD_CLASSIC_BUTTON_Y:     return "CL y";
        case WPAD_CLASSIC_BUTTON_PLUS:  return "CL +";
        case WPAD_CLASSIC_BUTTON_MINUS: return "CL -";
        case WPAD_CLASSIC_BUTTON_HOME:  return "CL Home";
        case WPAD_CLASSIC_BUTTON_FULL_L: return "CL L";
        case WPAD_CLASSIC_BUTTON_FULL_R: return "CL R";
        case WPAD_CLASSIC_BUTTON_ZL:    return "CL ZL";
        case WPAD_CLASSIC_BUTTON_ZR:    return "CL ZR";
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

typedef struct {
    WiiCtrlIconId icon;
    const char* label; // drawn after icon; may be NULL
} HintPart;

static float uiMeasureText(float scale, const char* text) {
    if (!text) return 0.0f;
    float width = 0.0f;
    float line = 0.0f;
    for (const char* p = text; *p; p++) {
        if (*p == '\n') {
            if (line > width) width = line;
            line = 0.0f;
            continue;
        }
        const DebugFontGlyphEntry* glyph = lookupGlyph((uint8_t)*p);
        if (glyph) line += (float)glyph->xadvance * scale;
    }
    if (line > width) width = line;
    return width;
}

static float drawHintPart(float x, float y, float iconScale, float textScale, const HintPart* part) {
    WiiCtrlIcons_draw(part->icon, x, y, iconScale, 255, 255, 255, 255);
    x += WiiCtrlIcons_advance(part->icon, iconScale) + 4.0f;
    if (part->label && part->label[0]) {
        uiDrawText(x, y + 2.0f, textScale, 160, 160, 180, 255, part->label);
        x += uiMeasureText(textScale, part->label) + 14.0f;
    }
    return x;
}

static void drawHintBar(int fbH, const HintPart* parts, int count) {
    float x = 56.0f;
    float y = (float)fbH - 54.0f;
    float iconScale = 0.52f; /* cells are 32px; keep on-screen size ~16px */
    float textScale = 0.42f;
    for (int i = 0; i < count; i++) {
        x = drawHintPart(x, y, iconScale, textScale, &parts[i]);
    }
}

static WiiCtrlIconId iconForWpad(uint32_t mask) {
    if (WII_INPUT_IS_GC(mask)) {
        switch (WII_INPUT_GC_PAD(mask)) {
            case PAD_BUTTON_UP:    return WII_CTRL_ICON_GC_DPAD_U;
            case PAD_BUTTON_DOWN:  return WII_CTRL_ICON_GC_DPAD_D;
            case PAD_BUTTON_LEFT:  return WII_CTRL_ICON_GC_DPAD_L;
            case PAD_BUTTON_RIGHT: return WII_CTRL_ICON_GC_DPAD_R;
            case PAD_BUTTON_A:     return WII_CTRL_ICON_GC_A;
            case PAD_BUTTON_B:     return WII_CTRL_ICON_GC_B;
            case PAD_BUTTON_X:     return WII_CTRL_ICON_GC_X;
            case PAD_BUTTON_Y:     return WII_CTRL_ICON_GC_Y;
            case PAD_BUTTON_START: return WII_CTRL_ICON_GC_START;
            case PAD_TRIGGER_Z:    return WII_CTRL_ICON_GC_Z;
            case PAD_TRIGGER_L:    return WII_CTRL_ICON_GC_L;
            case PAD_TRIGGER_R:    return WII_CTRL_ICON_GC_R;
            default:               return WII_CTRL_ICON_GAMECUBE;
        }
    }
    switch (mask) {
        case WPAD_BUTTON_UP:    return WII_CTRL_ICON_DPAD_U;
        case WPAD_BUTTON_DOWN:  return WII_CTRL_ICON_DPAD_D;
        case WPAD_BUTTON_LEFT:  return WII_CTRL_ICON_DPAD_L;
        case WPAD_BUTTON_RIGHT: return WII_CTRL_ICON_DPAD_R;
        case WPAD_BUTTON_A:     return WII_CTRL_ICON_WM_A;
        case WPAD_BUTTON_B:     return WII_CTRL_ICON_WM_B;
        case WPAD_BUTTON_PLUS:  return WII_CTRL_ICON_PLUS;
        case WPAD_BUTTON_MINUS: return WII_CTRL_ICON_MINUS;
        case WPAD_BUTTON_1:     return WII_CTRL_ICON_WM_1;
        case WPAD_BUTTON_2:     return WII_CTRL_ICON_WM_2;
        case WPAD_BUTTON_HOME:  return WII_CTRL_ICON_HOME;
        case WPAD_CLASSIC_BUTTON_UP:    return WII_CTRL_ICON_DPAD_U;
        case WPAD_CLASSIC_BUTTON_DOWN:  return WII_CTRL_ICON_DPAD_D;
        case WPAD_CLASSIC_BUTTON_LEFT:  return WII_CTRL_ICON_DPAD_L;
        case WPAD_CLASSIC_BUTTON_RIGHT: return WII_CTRL_ICON_DPAD_R;
        case WPAD_CLASSIC_BUTTON_A:     return WII_CTRL_ICON_CL_A;
        case WPAD_CLASSIC_BUTTON_B:     return WII_CTRL_ICON_CL_B;
        case WPAD_CLASSIC_BUTTON_X:     return WII_CTRL_ICON_CL_X;
        case WPAD_CLASSIC_BUTTON_Y:     return WII_CTRL_ICON_CL_Y;
        case WPAD_CLASSIC_BUTTON_PLUS:  return WII_CTRL_ICON_PLUS;
        case WPAD_CLASSIC_BUTTON_MINUS: return WII_CTRL_ICON_MINUS;
        case WPAD_CLASSIC_BUTTON_HOME:  return WII_CTRL_ICON_HOME;
        case WPAD_CLASSIC_BUTTON_FULL_L: return WII_CTRL_ICON_CL_L;
        case WPAD_CLASSIC_BUTTON_FULL_R: return WII_CTRL_ICON_CL_R;
        case WPAD_CLASSIC_BUTTON_ZL:    return WII_CTRL_ICON_CL_ZL;
        case WPAD_CLASSIC_BUTTON_ZR:    return WII_CTRL_ICON_CL_ZR;
        default:                return WII_CTRL_ICON_WIIMOTE;
    }
}

// Draws the panel chrome. Returns Y where screen content should start.
// hintParts/hintCount draw controller icons in the footer (NULL/0 = no footer).
static float drawMenuChrome(int fbW, int fbH, const char* title, const char* subtitle,
                            const HintPart* hintParts, int hintCount) {
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

    if (hintParts && hintCount > 0) {
        drawHintBar(fbH, hintParts, hintCount);
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
        // Capture first interesting button for the active preset.
        if (settings->controllerPreset == WII_CTRL_PRESET_GAMECUBE) {
            PAD_ScanPads();
            uint16_t padDown = PAD_ButtonsDown(0);
            static const uint16_t gcCapturable[] = {
                PAD_BUTTON_LEFT, PAD_BUTTON_RIGHT, PAD_BUTTON_UP, PAD_BUTTON_DOWN,
                PAD_BUTTON_A, PAD_BUTTON_B, PAD_BUTTON_X, PAD_BUTTON_Y,
                PAD_BUTTON_START, PAD_TRIGGER_Z, PAD_TRIGGER_L, PAD_TRIGGER_R,
            };
            for (int i = 0; i < (int)(sizeof(gcCapturable) / sizeof(gcCapturable[0])); i++) {
                if (padDown & gcCapturable[i]) {
                    uint32_t encoded = WII_INPUT_GC_FLAG | gcCapturable[i];
                    if (gUi.rebindIndex >= 0 && gUi.rebindIndex < settings->mapCount) {
                        for (int j = 0; j < settings->mapCount; j++) {
                            if (j != gUi.rebindIndex && settings->maps[j].wpadButton == encoded) {
                                settings->maps[j].wpadButton = settings->maps[gUi.rebindIndex].wpadButton;
                            }
                        }
                        settings->maps[gUi.rebindIndex].wpadButton = encoded;
                        *dirtySave = true;
                    }
                    gUi.waitingRebind = false;
                    gUi.screen = SCREEN_CONTROLS;
                    return ACT_NONE;
                }
            }
        } else {
            const uint32_t* capturable = NULL;
            int capturableCount = 0;
            static const uint32_t wmCapturable[] = {
                WPAD_BUTTON_LEFT, WPAD_BUTTON_RIGHT, WPAD_BUTTON_UP, WPAD_BUTTON_DOWN,
                WPAD_BUTTON_A, WPAD_BUTTON_B, WPAD_BUTTON_PLUS, WPAD_BUTTON_MINUS,
                WPAD_BUTTON_1, WPAD_BUTTON_2,
            };
            static const uint32_t clCapturable[] = {
                WPAD_CLASSIC_BUTTON_LEFT, WPAD_CLASSIC_BUTTON_RIGHT,
                WPAD_CLASSIC_BUTTON_UP, WPAD_CLASSIC_BUTTON_DOWN,
                WPAD_CLASSIC_BUTTON_A, WPAD_CLASSIC_BUTTON_B,
                WPAD_CLASSIC_BUTTON_X, WPAD_CLASSIC_BUTTON_Y,
                WPAD_CLASSIC_BUTTON_PLUS, WPAD_CLASSIC_BUTTON_MINUS,
                WPAD_CLASSIC_BUTTON_FULL_L, WPAD_CLASSIC_BUTTON_FULL_R,
                WPAD_CLASSIC_BUTTON_ZL, WPAD_CLASSIC_BUTTON_ZR,
            };
            if (settings->controllerPreset == WII_CTRL_PRESET_CLASSIC) {
                capturable = clCapturable;
                capturableCount = (int)(sizeof(clCapturable) / sizeof(clCapturable[0]));
            } else {
                capturable = wmCapturable;
                capturableCount = (int)(sizeof(wmCapturable) / sizeof(wmCapturable[0]));
            }
            for (int i = 0; i < capturableCount; i++) {
                if (down & capturable[i]) {
                    if (gUi.rebindIndex >= 0 && gUi.rebindIndex < settings->mapCount) {
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
                if (!inGame && gUi.gameCount > 1) {
                    gUi.screen = SCREEN_GAMES;
                    gUi.cursor = gUi.gameIndex;
                }
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
            /* cursor: 0 = preset, 1..mapCount = bindings, then reset, back */
            int items = settings->mapCount + 3;
            if (gUi.cursor >= items) gUi.cursor = items - 1;
            if (down & WPAD_BUTTON_B) {
                gUi.screen = SCREEN_MAIN;
                gUi.cursor = inGame ? 0 : 2;
            }
            if (gUi.cursor == 0) {
                if (down & WPAD_BUTTON_LEFT) {
                    int p = settings->controllerPreset - 1;
                    if (p < 0) p = WII_CTRL_PRESET_COUNT - 1;
                    WiiSettings_applyPreset(settings, p);
                    *dirtySave = true;
                }
                if (down & WPAD_BUTTON_RIGHT) {
                    int p = settings->controllerPreset + 1;
                    if (p >= WII_CTRL_PRESET_COUNT) p = 0;
                    WiiSettings_applyPreset(settings, p);
                    *dirtySave = true;
                }
            }
            if (down & (WPAD_BUTTON_A | WPAD_BUTTON_PLUS)) {
                if (gUi.cursor == 0) {
                    /* A on preset row also cycles forward */
                    int p = settings->controllerPreset + 1;
                    if (p >= WII_CTRL_PRESET_COUNT) p = 0;
                    WiiSettings_applyPreset(settings, p);
                    *dirtySave = true;
                } else if (gUi.cursor >= 1 && gUi.cursor <= settings->mapCount) {
                    gUi.rebindIndex = gUi.cursor - 1;
                    gUi.waitingRebind = true;
                    gUi.screen = SCREEN_REBIND;
                } else if (gUi.cursor == settings->mapCount + 1) {
                    WiiSettings_applyPreset(settings, settings->controllerPreset);
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
        case SCREEN_GAMES: {
            int items = gUi.gameCount > 0 ? gUi.gameCount : 1;
            if (gUi.cursor >= items) gUi.cursor = items - 1;
            if (down & (WPAD_BUTTON_A | WPAD_BUTTON_PLUS)) {
                gUi.gameIndex = gUi.cursor;
                gUi.screen = SCREEN_MAIN;
                gUi.cursor = 0;
            }
            if (down & WPAD_BUTTON_B) {
                return ACT_EXIT_TO_WII_MENU;
            }
            break;
        }
    }
    return ACT_NONE;
}

static void drawScreen(int fbW, int fbH, const WiiPortSettings* settings, bool inGame) {
    char line[160];
    float x = 64.0f;
    float y;

    if (gUi.waitingRebind || gUi.screen == SCREEN_REBIND) {
        HintPart rebindHint[2];
        if (settings->controllerPreset == WII_CTRL_PRESET_GAMECUBE) {
            rebindHint[0].icon = WII_CTRL_ICON_GAMECUBE;
            rebindHint[0].label = " press a button";
        } else if (settings->controllerPreset == WII_CTRL_PRESET_CLASSIC) {
            rebindHint[0].icon = WII_CTRL_ICON_CLASSIC;
            rebindHint[0].label = " press a button";
        } else {
            rebindHint[0].icon = WII_CTRL_ICON_WIIMOTE;
            rebindHint[0].label = " press a button";
        }
        rebindHint[1].icon = WII_CTRL_ICON_HOME;
        rebindHint[1].label = " cancel";
        y = drawMenuChrome(fbW, fbH, "Rebind", NULL, rebindHint, 2);
        if (gUi.rebindIndex >= 0 && gUi.rebindIndex < settings->mapCount) {
            snprintf(line, sizeof(line), "Remapping: %s", gmlKeyName(settings->maps[gUi.rebindIndex].gmlKey));
            uiDrawText(x, y, UI_TEXT_SCALE, 255, 255, 255, 255, line);
            snprintf(line, sizeof(line), "Waiting for %s button...",
                     WiiSettings_presetName(settings->controllerPreset));
            uiDrawText(x, y + UI_LINE * 2, UI_TEXT_SCALE, 200, 200, 210, 255, line);
        }
        return;
    }

    switch (gUi.screen) {
        case SCREEN_GAMES: {
            static const HintPart gameHint[] = {
                { WII_CTRL_ICON_DPAD, " select" },
                { WII_CTRL_ICON_WM_A, " confirm" },
            };
            y = drawMenuChrome(fbW, fbH, "Games", "Converted packages on this SD", gameHint, 2);
            if (gUi.gameCount <= 0) {
                uiDrawText(x, y, UI_TEXT_SCALE, 200, 200, 210, 255, "No games found.");
            } else {
                for (int i = 0; i < gUi.gameCount; i++) {
                    drawSelectable(x, y + UI_LINE * (float)i, gUi.cursor == i, gUi.games[i].title);
                }
            }
            break;
        }
        case SCREEN_MAIN: {
            static const HintPart mainHint[] = {
                { WII_CTRL_ICON_DPAD, " move" },
                { WII_CTRL_ICON_WM_A, " confirm" },
                { WII_CTRL_ICON_WM_B, " back" },
            };
            y = drawMenuChrome(fbW, fbH,
                               inGame ? "Butterscotch  |  System" : "Butterscotch",
                               inGame ? "Game paused"
                                      : (gUi.gameCount > 0 ? gUi.games[gUi.gameIndex].title
                                                           : "Wii GameMaker runner"),
                               mainHint, 3);
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
        }
        case SCREEN_OPTIONS: {
            static const HintPart optHint[] = {
                { WII_CTRL_ICON_DPAD, " adjust" },
                { WII_CTRL_ICON_WM_B, " back" },
            };
            y = drawMenuChrome(fbW, fbH, "Options", NULL, optHint, 2);
            snprintf(line, sizeof(line), "Master Volume ...... %3d%%", (int)(settings->masterGain * 100.0f + 0.5f));
            drawSelectable(x, y + UI_LINE * 0, gUi.cursor == 0, line);
            snprintf(line, sizeof(line), "Debug Overlay ...... %s", overlayName(settings->debugOverlay));
            drawSelectable(x, y + UI_LINE * 1, gUi.cursor == 1, line);
            drawSelectable(x, y + UI_LINE * 2, gUi.cursor == 2, "Back");
            break;
        }
        case SCREEN_CONTROLS: {
            static const HintPart ctrlHint[] = {
                { WII_CTRL_ICON_DPAD, " preset" },
                { WII_CTRL_ICON_WM_A, " rebind" },
                { WII_CTRL_ICON_WM_B, " back" },
            };
            y = drawMenuChrome(fbW, fbH, "Controls", NULL, ctrlHint, 3);
            {
                float px = x;
                float py = y - UI_LINE * 0.15f;
                const float padScale = 0.55f;
                WiiCtrlIconId activePad = WII_CTRL_ICON_WIIMOTE;
                bool wmActive = settings->controllerPreset == WII_CTRL_PRESET_WIIMOTE_VERT
                             || settings->controllerPreset == WII_CTRL_PRESET_WIIMOTE_HORIZ;
                if (settings->controllerPreset == WII_CTRL_PRESET_GAMECUBE) {
                    activePad = WII_CTRL_ICON_GAMECUBE;
                } else if (settings->controllerPreset == WII_CTRL_PRESET_CLASSIC) {
                    activePad = WII_CTRL_ICON_CLASSIC;
                }
                WiiCtrlIcons_draw(WII_CTRL_ICON_WIIMOTE, px, py, padScale,
                                  wmActive ? 255 : 120, wmActive ? 255 : 120, wmActive ? 255 : 120, 255);
                px += WiiCtrlIcons_advance(WII_CTRL_ICON_WIIMOTE, padScale) + 6.0f;
                WiiCtrlIcons_draw(WII_CTRL_ICON_GAMECUBE, px, py, padScale,
                                  activePad == WII_CTRL_ICON_GAMECUBE ? 255 : 120,
                                  activePad == WII_CTRL_ICON_GAMECUBE ? 255 : 120,
                                  activePad == WII_CTRL_ICON_GAMECUBE ? 255 : 120, 255);
                px += WiiCtrlIcons_advance(WII_CTRL_ICON_GAMECUBE, padScale) + 6.0f;
                WiiCtrlIcons_draw(WII_CTRL_ICON_CLASSIC, px, py, padScale,
                                  activePad == WII_CTRL_ICON_CLASSIC ? 255 : 120,
                                  activePad == WII_CTRL_ICON_CLASSIC ? 255 : 120,
                                  activePad == WII_CTRL_ICON_CLASSIC ? 255 : 120, 255);
                y += UI_LINE * 1.1f;
            }
            snprintf(line, sizeof(line), "Preset  < %s >",
                     WiiSettings_presetName(settings->controllerPreset));
            drawSelectable(x, y, gUi.cursor == 0, line);
            for (int i = 0; i < settings->mapCount; i++) {
                float rowY = y + UI_LINE * (float)(i + 1);
                const float rowIconScale = 0.50f;
                WiiCtrlIconId ic = iconForWpad(settings->maps[i].wpadButton);
                WiiCtrlIcons_draw(ic, x, rowY, rowIconScale, 255, 255, 255, 255);
                snprintf(line, sizeof(line), "%-12s -> %s",
                         buttonName(settings->maps[i].wpadButton),
                         gmlKeyName(settings->maps[i].gmlKey));
                drawSelectable(x + 22.0f, rowY, gUi.cursor == i + 1, line);
            }
            drawSelectable(x, y + UI_LINE * (float)(settings->mapCount + 1),
                           gUi.cursor == settings->mapCount + 1, "Reset preset defaults");
            drawSelectable(x, y + UI_LINE * (float)(settings->mapCount + 2),
                           gUi.cursor == settings->mapCount + 2, "Back");
            if (settings->controllerPreset == WII_CTRL_PRESET_GAMECUBE
                || settings->controllerPreset == WII_CTRL_PRESET_CLASSIC) {
                uiDrawText(x, y + UI_LINE * (float)(settings->mapCount + 3.4f), 0.40f,
                           150, 160, 180, 255, "Left stick also moves (with D-pad).");
            }
            break;
        }
        case SCREEN_EXTRAS: {
            static const HintPart extrasHint[] = {
                { WII_CTRL_ICON_DPAD, " change room" },
                { WII_CTRL_ICON_WM_B, " back" },
            };
            y = drawMenuChrome(fbW, fbH, "Extras", NULL, extrasHint, 2);
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
        }
        case SCREEN_ABOUT: {
            static const HintPart aboutHint[] = {
                { WII_CTRL_ICON_WM_A, " / " },
                { WII_CTRL_ICON_WM_B, " back" },
            };
            y = drawMenuChrome(fbW, fbH, "About", NULL, aboutHint, 2);
            uiDrawText(x, y, UI_TEXT_SCALE, 230, 230, 240, 255, "Butterscotch on Wii");
            uiDrawText(x, y + UI_LINE * 1.2f, 0.45f, 200, 200, 210, 255,
                       "GameMaker runner port — GX renderer, AESND audio,");
            uiDrawText(x, y + UI_LINE * 2.2f, 0.45f, 200, 200, 210, 255,
                       "WTL1 tiled atlases for full-res sprites.");
            uiDrawText(x, y + UI_LINE * 3.5f, 0.45f, 200, 200, 210, 255,
                       "Fork playground. Not affiliated with Toby Fox.");
            uiDrawText(x, y + UI_LINE * 5.0f, 0.40f, 160, 165, 180, 255,
                       "Icons: Openclipart + Kenney (CC0) + Zacksly GC (CC BY).");
            uiDrawText(x, y + UI_LINE * 5.9f, 0.40f, 160, 165, 180, 255,
                       "HOME in-game opens this system menu.");
            drawSelectable(x, y + UI_LINE * 7.5f, true, "Back");
            break;
        }
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
    gUi.screen = (!inGame && gUi.gameCount > 1) ? SCREEN_GAMES : SCREEN_MAIN;
    gUi.cursor = (!inGame && gUi.gameCount > 1) ? gUi.gameIndex : 0;
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
    WiiPortSettings* settings,
    const WiiGameEntry* games,
    int gameCount,
    int* selectedGame
) {
    WPAD_Init();
    PAD_Init();
    gUi.games = games;
    gUi.gameCount = gameCount;
    gUi.gameIndex = (selectedGame && *selectedGame >= 0 && *selectedGame < gameCount)
        ? *selectedGame : 0;
    logInfo("WiiBootMenu: entering pre-boot shell (%d game%s)\n",
            gameCount, gameCount == 1 ? "" : "s");
    MenuAction act = runMenuLoop(rmode, xfb0, xfb1, fbIndex, bundleDir, settings, false, NULL);
    if (selectedGame) *selectedGame = gUi.gameIndex;
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
