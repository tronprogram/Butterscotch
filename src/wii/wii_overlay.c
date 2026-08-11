#include "wii_overlay.h"
#include "gx_renderer.h"
#include "debug_font.h"
#include "utils.h"
#include "stdio_compat.h"
#include "string_compat.h"
#include "stb_ds.h"

#ifdef USE_WII_AUDIO
#include "wii_audio_system.h"
#endif

#include <gccore.h>
#include <malloc.h>
#include <ogc/system.h>
#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_VM_GML_PROFILER
#include "profiler.h"
#endif

#define OVERLAY_LINE_HEIGHT_SCALE 0.80f
#define OVERLAY_TEXT_SCALE 0.55f
#define PROFILER_WINDOW_FRAMES 60

typedef struct {
    bool initialized;
    bool arenasSnapshotted;
    WiiDebugOverlayState state;
    GXTexObj fontTex;
    uint8_t* fontTexData;
    u32 mem1SnapLo;
    u32 mem1SnapHi;
    u32 mem2SnapLo;
    u32 mem2SnapHi;
    float fpsEma;
    int profilerFramesInWindow;
#ifdef ENABLE_VM_GML_PROFILER
    char profilerOverlayText[4096];
#endif
} WiiOverlay;

static WiiOverlay gOverlay = { 0 };

void WiiOverlay_snapshotArenas(void) {
    gOverlay.mem1SnapLo = (u32)SYS_GetArena1Lo();
    gOverlay.mem1SnapHi = (u32)SYS_GetArena1Hi();
    gOverlay.mem2SnapLo = (u32)SYS_GetArena2Lo();
    gOverlay.mem2SnapHi = (u32)SYS_GetArena2Hi();
    gOverlay.arenasSnapshotted = true;
}

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
                    tile[i * 2 + 0] = p[3]; // A
                    tile[i * 2 + 1] = p[0]; // R
                    tile[32 + i * 2 + 0] = p[1]; // G
                    tile[32 + i * 2 + 1] = p[2]; // B
                }
            }
        }
    }
}

static const DebugFontGlyphEntry* lookupGlyph(uint8_t c) {
    if (c < DEBUGFONT_FIRST_CP || c > DEBUGFONT_LAST_CP) return NULL;
    return &debugFontGlyphs[c - DEBUGFONT_FIRST_CP];
}

static void setupOverlayGx(int fbWidth, int fbHeight) {
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
}

static void drawText(float x, float y, float scale, uint8_t r, uint8_t g, uint8_t b, uint8_t a, const char* text) {
    if (text == NULL) return;

    GX_LoadTexObj(&gOverlay.fontTex, GX_TEXMAP0);

    float cursorY = y;
    int32_t len = (int32_t)strlen(text);
    int32_t lineStart = 0;

    for (int32_t i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            int32_t lineLen = i - lineStart;
            float pen = x;

            // Count quads first
            int quadCount = 0;
            for (int32_t j = 0; j < lineLen; j++) {
                const DebugFontGlyphEntry* glyph = lookupGlyph((uint8_t)text[lineStart + j]);
                if (glyph && glyph->w > 0 && glyph->h > 0) quadCount++;
            }

            if (quadCount > 0) {
                GX_Begin(GX_QUADS, GX_VTXFMT0, (u16)(quadCount * 4));
                for (int32_t j = 0; j < lineLen; j++) {
                    const DebugFontGlyphEntry* glyph = lookupGlyph((uint8_t)text[lineStart + j]);
                    if (!glyph) continue;

                    if (glyph->w > 0 && glyph->h > 0) {
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
                    }

                    pen += (float)glyph->xadvance * scale;
                }
                GX_End();
            } else {
                for (int32_t j = 0; j < lineLen; j++) {
                    const DebugFontGlyphEntry* glyph = lookupGlyph((uint8_t)text[lineStart + j]);
                    if (glyph) pen += (float)glyph->xadvance * scale;
                }
            }

            cursorY += (float)DEBUGFONT_LINE_HEIGHT * scale * OVERLAY_LINE_HEIGHT_SCALE;
            lineStart = i + 1;
        }
    }
}

void WiiOverlay_init(void) {
    if (gOverlay.initialized) return;

    size_t linearBytes = (size_t)DEBUGFONT_ATLAS_W * (size_t)DEBUGFONT_ATLAS_H * 4;
    uint8_t* linear = (uint8_t*)safeMalloc(linearBytes);
    for (int i = 0; i < DEBUGFONT_ATLAS_W * DEBUGFONT_ATLAS_H; i++) {
        uint8_t a = debugFontPixels[i];
        linear[i * 4 + 0] = 0xFF;
        linear[i * 4 + 1] = 0xFF;
        linear[i * 4 + 2] = 0xFF;
        linear[i * 4 + 3] = a;
    }

    gOverlay.fontTexData = (uint8_t*)memalign(32, linearBytes);
    if (!gOverlay.fontTexData) {
        free(linear);
        logWarn("WiiOverlay: memalign failed for debug font atlas\n");
        return;
    }
    convertLinearRgba8ToTiled(gOverlay.fontTexData, linear, DEBUGFONT_ATLAS_W, DEBUGFONT_ATLAS_H);
    free(linear);
    DCFlushRange(gOverlay.fontTexData, linearBytes);

    GX_InitTexObj(&gOverlay.fontTex, gOverlay.fontTexData,
                  DEBUGFONT_ATLAS_W, DEBUGFONT_ATLAS_H,
                  GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&gOverlay.fontTex, GX_NEAR, GX_NEAR);

    gOverlay.state = WII_STATS_ENABLED;
    gOverlay.profilerFramesInWindow = 0;
    gOverlay.fpsEma = 0.0f;
    if (!gOverlay.arenasSnapshotted) {
        WiiOverlay_snapshotArenas();
    }
    gOverlay.initialized = true;
    logInfo("WiiOverlay: debug stats overlay ready (HOME to cycle)\n");
}

void WiiOverlay_deinit(void) {
    if (!gOverlay.initialized) return;
    free(gOverlay.fontTexData);
    memset(&gOverlay, 0, sizeof(gOverlay));
}

WiiDebugOverlayState WiiOverlay_getDebugOverlayState(void) {
    if (!gOverlay.initialized) return WII_STATS_DISABLED;
    return gOverlay.state;
}

void WiiOverlay_setDebugOverlayState(WiiDebugOverlayState state, Runner* runner) {
    if (!gOverlay.initialized) return;
    gOverlay.state = state;
#ifdef ENABLE_VM_GML_PROFILER
    Profiler_setEnabled(&runner->vmContext->profiler, state == WII_STATS_ENABLED_WITH_PROFILER);
    gOverlay.profilerFramesInWindow = 0;
    gOverlay.profilerOverlayText[0] = '\0';
#else
    (void)runner;
#endif
}

void WiiOverlay_toggleDebugOverlay(Runner* runner) {
    if (!gOverlay.initialized) return;
    gOverlay.state = (WiiDebugOverlayState)((gOverlay.state + 1) % WII_STATS_MAX);
#ifdef ENABLE_VM_GML_PROFILER
    Profiler_setEnabled(&runner->vmContext->profiler, gOverlay.state == WII_STATS_ENABLED_WITH_PROFILER);
    gOverlay.profilerFramesInWindow = 0;
    gOverlay.profilerOverlayText[0] = '\0';
#else
    (void)runner;
#endif
}

void WiiOverlay_drawDebugOverlay(Renderer* renderer, const Runner* runner,
                                 float tickMs, float stepMs, float drawMs, float audioMs,
                                 float frameDeltaMs,
                                 int fbWidth, int fbHeight) {
    if (!gOverlay.initialized) return;
    if (gOverlay.state == WII_STATS_DISABLED) return;

    GxTextureStats tex = { 0 };
    GxRenderer_queryTextureStats(renderer, &tex);

#ifdef USE_WII_AUDIO
    WiiAudioStats audio = { 0 };
    WiiAudioSystem_queryStats(runner->audioSystem, &audio);
    int activeVoices = audio.activeVoices;
    int streamingVoices = audio.streamingVoices;
    double audioCacheMb = (double)audio.cachedPcmBytes / (1024.0 * 1024.0);
#else
    int activeVoices = 0;
    int streamingVoices = 0;
    double audioCacheMb = 0.0;
#endif

    // Arena accounting: totals are the post-XFB heap budget captured by snapshotArenas().
    // Used is how far Lo has advanced since then. mallinfo is intentionally unused (MEM1↔MEM2 sbrk lies).
    u32 mem1Lo = (u32)SYS_GetArena1Lo();
    u32 mem1Hi = (u32)SYS_GetArena1Hi();
    u32 mem2Lo = (u32)SYS_GetArena2Lo();
    u32 mem2Hi = (u32)SYS_GetArena2Hi();
    u32 mem1Total = gOverlay.mem1SnapHi > gOverlay.mem1SnapLo
        ? (gOverlay.mem1SnapHi - gOverlay.mem1SnapLo) : 0;
    u32 mem2Total = gOverlay.mem2SnapHi > gOverlay.mem2SnapLo
        ? (gOverlay.mem2SnapHi - gOverlay.mem2SnapLo) : 0;
    u32 mem1Used = mem1Lo > gOverlay.mem1SnapLo ? (mem1Lo - gOverlay.mem1SnapLo) : 0;
    u32 mem2Used = mem2Lo > gOverlay.mem2SnapLo ? (mem2Lo - gOverlay.mem2SnapLo) : 0;
    u32 mem1Free = mem1Hi > mem1Lo ? (mem1Hi - mem1Lo) : 0;
    u32 mem2Free = mem2Hi > mem2Lo ? (mem2Hi - mem2Lo) : 0;

    double mem1UsedMb = (double)mem1Used / (1024.0 * 1024.0);
    double mem1TotalMb = (double)mem1Total / (1024.0 * 1024.0);
    double mem1FreeMb = (double)mem1Free / (1024.0 * 1024.0);
    double mem2UsedMb = (double)mem2Used / (1024.0 * 1024.0);
    double mem2TotalMb = (double)mem2Total / (1024.0 * 1024.0);
    double mem2FreeMb = (double)mem2Free / (1024.0 * 1024.0);
    double texMb = (double)tex.residentBytes / (1024.0 * 1024.0);

    float fpsInstant = 0.0f;
    if (frameDeltaMs > 0.01f) fpsInstant = 1000.0f / frameDeltaMs;
    if (gOverlay.fpsEma <= 0.0f) gOverlay.fpsEma = fpsInstant;
    else gOverlay.fpsEma = gOverlay.fpsEma * 0.9f + fpsInstant * 0.1f;

    const char* roomName = (runner->currentRoom && runner->currentRoom->name)
        ? runner->currentRoom->name : "?";
    uint32_t roomSpeed = runner->currentRoom ? runner->currentRoom->speed : 0;

    char debugText[704];
    snprintf(debugText, sizeof(debugText),
        "Room: %s\n"
        "FPS: %.1f  (cap %u)\n"
        "Tick: %.2fms  Step: %.2fms\n"
        "Draw: %.2fms  Audio: %.2fms\n"
        "MEM1: %.1f/%.1f MB  (free %.1f)\n"
        "MEM2: %.1f/%.1f MB  (free %.1f)\n"
        "TXTR: %u/%u pages  %.1f MB\n"
        "Audio: %d voice (%d stream)  cache %.2f MB\n"
        "Instances: %d  Structs: %d\n"
        "HOME: cycle overlay",
        roomName,
        (double)gOverlay.fpsEma, roomSpeed,
        (double)tickMs, (double)stepMs,
        (double)drawMs, (double)audioMs,
        mem1UsedMb, mem1TotalMb, mem1FreeMb,
        mem2UsedMb, mem2TotalMb, mem2FreeMb,
        tex.loadedPages, tex.totalPages, texMb,
        activeVoices, streamingVoices, audioCacheMb,
        (int)arrlen(runner->instances), (int)arrlen(runner->structInstances));

    setupOverlayGx(fbWidth, fbHeight);
    drawText(11.0f, 11.0f, OVERLAY_TEXT_SCALE, 0, 0, 0, 180, debugText);
    drawText(10.0f, 10.0f, OVERLAY_TEXT_SCALE, 255, 255, 255, 255, debugText);

    if (gOverlay.state == WII_STATS_ENABLED_WITH_PROFILER) {
        float profilerY = 10.0f + ((float)DEBUGFONT_LINE_HEIGHT * OVERLAY_TEXT_SCALE * OVERLAY_LINE_HEIGHT_SCALE * 11.0f) + 6.0f;
#ifdef ENABLE_VM_GML_PROFILER
        gOverlay.profilerFramesInWindow++;
        if (gOverlay.profilerFramesInWindow >= PROFILER_WINDOW_FRAMES) {
            char* profilerReport = Profiler_createReport(runner->vmContext->profiler, 25, gOverlay.profilerFramesInWindow);
            if (profilerReport) {
                snprintf(gOverlay.profilerOverlayText, sizeof(gOverlay.profilerOverlayText), "%s", profilerReport);
                free(profilerReport);
            }
            Profiler_reset(runner->vmContext->profiler);
            gOverlay.profilerFramesInWindow = 0;
        }
        const char* profilerDisplay = gOverlay.profilerOverlayText[0] != '\0'
            ? gOverlay.profilerOverlayText
            : "GML Profiler (collecting...)";
        drawText(11.0f, profilerY + 1.0f, 0.35f, 0, 0, 0, 180, profilerDisplay);
        drawText(10.0f, profilerY, 0.35f, 255, 255, 180, 255, profilerDisplay);
#else
        const char* disabled = "GML Profiler disabled in this build";
        drawText(11.0f, profilerY + 1.0f, 0.35f, 0, 0, 0, 180, disabled);
        drawText(10.0f, profilerY, 0.35f, 255, 200, 120, 255, disabled);
#endif
    }
}
