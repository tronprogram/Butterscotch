// Standalone Wii GX graphics stress demo for Butterscotch.
// No data.win / Runner — isolates atlas + sprite draw techniques.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

#define GX_FIFO_SIZE (256 * 1024)
#define MAX_TEX_DIM 1024
#define DEMO_SPRITES_MAX 1024

typedef struct {
    GXTexObj obj;
    uint8_t* data;
    int32_t y;
    int32_t height;
} DemoSlice;

typedef struct {
    DemoSlice* slices;
    int sliceCount;
    int width;
    int height;
    int scale; // 1 = native upload space; 2 = source coords are 2x uploaded
    const char* name;
} DemoAtlas;

typedef struct {
    float x, y;
    float vx, vy;
    float originX, originY;
    float scaleX, scaleY;
    float angleDeg;
    int atlasId;
    int srcX, srcY, srcW, srcH; // in original atlas coords
    uint8_t r, g, b, a;
} DemoSprite;

static GXRModeObj* rmode;
static void* xfb[2];
static u32 fb = 0;
static void* fifoBuffer;

static DemoAtlas atlases[3];
static DemoSprite sprites[DEMO_SPRITES_MAX];
int spriteCount = 64;
static float camX = 0.0f;
static float camY = 0.0f;
static float scrollSpeed = 1.5f;
static bool enableTall = true;
static bool enableWide = true;
static bool enableTiled = true;
static bool enableCrossSlice = true;
static bool running = true;

static float fpsEma = 0.0f;
static float lastDrawMs = 0.0f;

// --- small helpers ---

static uint64_t nowUs(void) {
    return ticks_to_microsecs(gettime());
}

static void setOrthoPixel(void) {
    Mtx44 proj;
    guOrtho(proj, 0.0f, (f32)rmode->efbHeight, 0.0f, (f32)rmode->fbWidth, 0.0f, 300.0f);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    Mtx mv;
    guMtxIdentity(mv);
    guMtxTransApply(mv, mv, 0.0f, 0.0f, -5.0f);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
}

static void emitTexQuad(
    float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
    float u0, float v0, float u1, float v1,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position2f32(x0, y0); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u0, v0);
        GX_Position2f32(x1, y1); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u1, v0);
        GX_Position2f32(x2, y2); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u1, v1);
        GX_Position2f32(x3, y3); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u0, v1);
    GX_End();
}

static void emitSolidQuad(
    float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_DISABLE, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position2f32(x0, y0); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0, 0);
        GX_Position2f32(x1, y1); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0, 0);
        GX_Position2f32(x2, y2); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0, 0);
        GX_Position2f32(x3, y3); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0, 0);
    GX_End();
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
}

// --- atlas creation (linear RGBA -> GX RGBA8 tiled slices) ---

static void rgbaToGxTiled(uint8_t* dst, const uint8_t* src, int srcW, int srcH,
                          int sliceY, int sliceH, int dstW)
{
    int tilesX = dstW / 4;
    int tilesY = sliceH / 4;
    memset(dst, 0, (size_t)dstW * (size_t)sliceH * 4u);
    for (int ty = 0; ty < tilesY; ty++) {
        for (int tx = 0; tx < tilesX; tx++) {
            uint8_t* tile = dst + (ty * tilesX + tx) * 64;
            for (int row = 0; row < 4; row++) {
                for (int col = 0; col < 4; col++) {
                    int px = tx * 4 + col;
                    int py = sliceY + ty * 4 + row;
                    uint8_t R = 0, G = 0, B = 0, A = 0;
                    if (px < srcW && py < srcH) {
                        const uint8_t* p = src + (py * srcW + px) * 4;
                        R = p[0]; G = p[1]; B = p[2]; A = p[3];
                    }
                    int idx = row * 4 + col;
                    tile[idx * 2] = A;
                    tile[idx * 2 + 1] = R;
                    tile[32 + idx * 2] = G;
                    tile[32 + idx * 2 + 1] = B;
                }
            }
        }
    }
}

static uint8_t* makePatternRGBA(int w, int h, int cell, uint8_t baseR, uint8_t baseG, uint8_t baseB) {
    uint8_t* px = (uint8_t*)memalign(32, (size_t)w * (size_t)h * 4u);
    if (!px) return NULL;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int cx = x / cell;
            int cy = y / cell;
            int checker = (cx + cy) & 1;
            uint8_t* p = px + (y * w + x) * 4;
            p[0] = checker ? baseR : (uint8_t)(baseR / 3);
            p[1] = checker ? baseG : (uint8_t)(baseG / 3);
            p[2] = checker ? baseB : (uint8_t)(baseB / 3);
            p[3] = 255;
            // Punch a translucent diamond in each cell for blend testing.
            int lx = x % cell;
            int ly = y % cell;
            int dx = lx - cell / 2;
            int dy = ly - cell / 2;
            if (abs(dx) + abs(dy) < cell / 4) {
                p[3] = 160;
                p[0] = 255;
                p[1] = 255;
                p[2] = 255;
            }
        }
    }
    return px;
}

static uint8_t* downsampleNN(const uint8_t* src, int w, int h, int scale, int* outW, int* outH) {
    int dw = w / scale;
    int dh = h / scale;
    uint8_t* dst = (uint8_t*)memalign(32, (size_t)dw * (size_t)dh * 4u);
    if (!dst) return NULL;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            const uint8_t* s = src + ((y * scale) * w + (x * scale)) * 4;
            uint8_t* d = dst + (y * dw + x) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
    *outW = dw;
    *outH = dh;
    return dst;
}

static bool uploadAtlas(DemoAtlas* atlas, uint8_t* rgba, int w, int h, int scale, const char* name) {
    int pw = (w + 3) & ~3;
    int ph = (h + 3) & ~3;
    if (pw > MAX_TEX_DIM) {
        printf("Atlas %s still too wide (%d)\n", name, pw);
        return false;
    }
    int sliceCount = (ph + MAX_TEX_DIM - 1) / MAX_TEX_DIM;
    DemoSlice* slices = (DemoSlice*)calloc((size_t)sliceCount, sizeof(DemoSlice));
    if (!slices) return false;

    for (int si = 0; si < sliceCount; si++) {
        DemoSlice* slice = &slices[si];
        slice->y = si * MAX_TEX_DIM;
        slice->height = ph - slice->y;
        if (slice->height > MAX_TEX_DIM) slice->height = MAX_TEX_DIM;
        size_t bytes = (size_t)pw * (size_t)slice->height * 4u;
        slice->data = (uint8_t*)memalign(32, bytes);
        if (!slice->data) {
            printf("memalign failed for %s slice %d (%zu bytes)\n", name, si, bytes);
            return false;
        }
        rgbaToGxTiled(slice->data, rgba, w, h, slice->y, slice->height, pw);
        GX_InitTexObj(&slice->obj, slice->data, (u16)pw, (u16)slice->height,
                      GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjFilterMode(&slice->obj, GX_NEAR, GX_NEAR);
        DCFlushRange(slice->data, bytes);
    }

    atlas->slices = slices;
    atlas->sliceCount = sliceCount;
    atlas->width = pw;
    atlas->height = ph;
    atlas->scale = scale < 1 ? 1 : scale;
    atlas->name = name;
    printf("Loaded atlas '%s' upload=%dx%d scale=%d slices=%d (%.1f MB)\n",
           name, pw, ph, atlas->scale, sliceCount,
           (double)pw * (double)ph * 4.0 / (1024.0 * 1024.0));
    return true;
}

static bool createAtlases(void) {
    // 0: 1024x1024
    {
        uint8_t* rgba = makePatternRGBA(1024, 1024, 32, 80, 180, 255);
        if (!rgba || !uploadAtlas(&atlases[0], rgba, 1024, 1024, 1, "square1024")) return false;
        free(rgba);
    }
    // 1: 1024x2048 tall (vertical slices)
    {
        uint8_t* rgba = makePatternRGBA(1024, 2048, 32, 80, 255, 120);
        if (!rgba || !uploadAtlas(&atlases[1], rgba, 1024, 2048, 1, "tall1024x2048")) return false;
        free(rgba);
    }
    // 2: 2048x2048 wide -> downscale 2x
    {
        uint8_t* rgba = makePatternRGBA(2048, 2048, 64, 255, 120, 80);
        if (!rgba) return false;
        int dw = 0, dh = 0;
        uint8_t* scaled = downsampleNN(rgba, 2048, 2048, 2, &dw, &dh);
        free(rgba);
        if (!scaled || !uploadAtlas(&atlases[2], scaled, dw, dh, 2, "wide2048_down2")) return false;
        free(scaled);
    }
    GX_InvalidateTexAll();
    return true;
}

// --- draw path mirroring Butterscotch emitTexturedAtlasRect ---

static void drawAtlasRect(const DemoAtlas* atlas,
                          int sourceX, int sourceY, int sourceW, int sourceH,
                          float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    int scale = atlas->scale < 1 ? 1 : atlas->scale;
    int mx = sourceX / scale;
    int my = sourceY / scale;
    int mw = sourceW / scale;
    int mh = sourceH / scale;
    if (mw < 1 && sourceW > 0) mw = 1;
    if (mh < 1 && sourceH > 0) mh = 1;
    if (mw <= 0 || mh <= 0) return;
    if (mx < 0 || my < 0) return;
    if (mx + mw > atlas->width || my + mh > atlas->height) return;

    float tw = (float)atlas->width;
    int yCursor = my;
    int yEnd = my + mh;

    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

    while (yCursor < yEnd) {
        int sliceIndex = yCursor / MAX_TEX_DIM;
        if (sliceIndex < 0 || sliceIndex >= atlas->sliceCount) break;
        DemoSlice* slice = &atlas->slices[sliceIndex];
        int sliceEnd = slice->y + slice->height;
        int segH = (yEnd < sliceEnd) ? (yEnd - yCursor) : (sliceEnd - yCursor);
        if (segH <= 0) break;

        float t0 = (float)(yCursor - my) / (float)mh;
        float t1 = (float)(yCursor + segH - my) / (float)mh;
        float sx0 = x0 + (x3 - x0) * t0;
        float sy0 = y0 + (y3 - y0) * t0;
        float sx1 = x1 + (x2 - x1) * t0;
        float sy1 = y1 + (y2 - y1) * t0;
        float sx2 = x1 + (x2 - x1) * t1;
        float sy2 = y1 + (y2 - y1) * t1;
        float sx3 = x0 + (x3 - x0) * t1;
        float sy3 = y0 + (y3 - y0) * t1;

        float th = (float)slice->height;
        float u0 = (float)mx / tw;
        float u1 = (float)(mx + mw) / tw;
        float v0 = (float)(yCursor - slice->y) / th;
        float v1 = (float)(yCursor + segH - slice->y) / th;

        GX_LoadTexObj(&slice->obj, GX_TEXMAP0);
        emitTexQuad(sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3, u0, v0, u1, v1, r, g, b, a);
        yCursor += segH;
    }
}

static void worldToScreen(float wx, float wy, float* sx, float* sy) {
    // Integer-expanded view with fractional camera (mirrors Butterscotch intent).
    float viewX = floorf(camX);
    float viewY = floorf(camY);
    float fracX = viewX + (camX - truncf(camX));
    float fracY = viewY + (camY - truncf(camY));
    *sx = (wx - fracX);
    *sy = (wy - fracY);
}

static void drawSprite(const DemoSprite* spr) {
    const DemoAtlas* atlas = &atlases[spr->atlasId];
    float lx0 = -spr->originX;
    float ly0 = -spr->originY;
    float lx1 = lx0 + (float)spr->srcW * spr->scaleX;
    float ly1 = ly0 + (float)spr->srcH * spr->scaleY;

    float ang = -spr->angleDeg * (float)M_PI / 180.0f;
    float c = cosf(ang), s = sinf(ang);
    float corners[4][2] = {
        { lx0, ly0 }, { lx1, ly0 }, { lx1, ly1 }, { lx0, ly1 }
    };
    float sx[4], sy[4];
    for (int i = 0; i < 4; i++) {
        float rx = corners[i][0] * c - corners[i][1] * s;
        float ry = corners[i][0] * s + corners[i][1] * c;
        worldToScreen(spr->x + rx, spr->y + ry, &sx[i], &sy[i]);
    }
    drawAtlasRect(atlas, spr->srcX, spr->srcY, spr->srcW, spr->srcH,
                  sx[0], sy[0], sx[1], sy[1], sx[2], sy[2], sx[3], sy[3],
                  spr->r, spr->g, spr->b, spr->a);
}

static void drawSpriteTiled(const DemoAtlas* atlas, int srcX, int srcY, int srcW, int srcH,
                            float originX, float originY, float x, float y,
                            float xscale, float yscale, float roomW, float roomH)
{
    float tileW = (float)srcW * fabsf(xscale);
    float tileH = (float)srcH * fabsf(yscale);
    if (tileW < 0.5f || tileH < 0.5f) return;

    float gridX = x - originX * fabsf(xscale);
    float gridY = y - originY * fabsf(yscale);
    float startX = fmodf(gridX, tileW);
    if (startX > 0.0f) startX -= tileW;
    float startY = fmodf(gridY, tileH);
    if (startY > 0.0f) startY -= tileH;

    for (float dy = startY; dy < roomH; dy += tileH) {
        for (float dx = startX; dx < roomW; dx += tileW) {
            float x0, y0, x1, y1, x2, y2, x3, y3;
            worldToScreen(dx, dy, &x0, &y0);
            worldToScreen(dx + tileW, dy, &x1, &y1);
            worldToScreen(dx + tileW, dy + tileH, &x2, &y2);
            worldToScreen(dx, dy + tileH, &x3, &y3);
            drawAtlasRect(atlas, srcX, srcY, srcW, srcH,
                          x0, y0, x1, y1, x2, y2, x3, y3, 255, 255, 255, 255);
        }
    }
}

static void seedSprites(void) {
    for (int i = 0; i < DEMO_SPRITES_MAX; i++) {
        DemoSprite* s = &sprites[i];
        int kind = i % 3;
        s->atlasId = kind;
        s->x = (float)(50 + (i * 37) % 700);
        s->y = (float)(40 + (i * 53) % 500);
        s->vx = ((i % 7) - 3) * 0.4f;
        s->vy = ((i % 5) - 2) * 0.35f;
        s->originX = 16;
        s->originY = 16;
        s->scaleX = 1.0f;
        s->scaleY = 1.0f;
        s->angleDeg = (float)(i % 360);
        // Pick cells in original atlas coordinates.
        if (kind == 0) { // 1024
            s->srcX = (i % 16) * 64;
            s->srcY = ((i / 16) % 16) * 64;
            s->srcW = 64; s->srcH = 64;
        } else if (kind == 1) { // tall 1024x2048 — intentionally cross the mid seam often
            s->srcX = (i % 8) * 64;
            s->srcY = 1000 + (i % 3) * 8; // crosses y=1024
            s->srcW = 64; s->srcH = 48;
        } else { // wide original 2048 coords (scale=2 on upload)
            s->srcX = (i % 16) * 128;
            s->srcY = ((i / 16) % 16) * 128;
            s->srcW = 128; s->srcH = 128;
        }
        s->r = 255; s->g = 255; s->b = 255; s->a = 230;
    }
}

static void updateSprites(void) {
    for (int i = 0; i < spriteCount; i++) {
        DemoSprite* s = &sprites[i];
        if (!enableTall && s->atlasId == 1) continue;
        if (!enableWide && s->atlasId == 2) continue;
        s->x += s->vx;
        s->y += s->vy;
        s->angleDeg += 0.4f;
        if (s->x < 0 || s->x > 800) s->vx = -s->vx;
        if (s->y < 0 || s->y > 600) s->vy = -s->vy;
    }
    camX += scrollSpeed;
    if (camX > 200.0f || camX < -40.0f) scrollSpeed = -scrollSpeed;
}

static void drawHud(void) {
    // Simple bar meters / blocks as "text stand-in" so we don't need a font atlas.
    // Left stack of colored status quads + numeric bars via width.
    float x = 8, y = 8;
    emitSolidQuad(x, y, x + 220, y, x + 220, y + 78, x, y + 78, 0, 0, 0, 180);
    // FPS bar (green): 0..60 maps to width
    float fps = fpsEma;
    float fpsW = fminf(fps, 60.0f) / 60.0f * 200.0f;
    emitSolidQuad(x + 8, y + 8, x + 8 + fpsW, y + 8, x + 8 + fpsW, y + 20, x + 8, y + 20, 80, 220, 80, 255);
    // Draw-ms bar (red): 0..50ms
    float dW = fminf(lastDrawMs, 50.0f) / 50.0f * 200.0f;
    emitSolidQuad(x + 8, y + 28, x + 8 + dW, y + 28, x + 8 + dW, y + 40, x + 8, y + 40, 220, 80, 80, 255);
    // Sprite count bar (blue)
    float sW = (float)spriteCount / (float)DEMO_SPRITES_MAX * 200.0f;
    emitSolidQuad(x + 8, y + 48, x + 8 + sW, y + 48, x + 8 + sW, y + 60, x + 8, y + 60, 80, 120, 255, 255);
    // Feature flags as dots
    if (enableTall) emitSolidQuad(x + 8, y + 66, x + 18, y + 66, x + 18, y + 74, x + 8, y + 74, 80, 255, 120, 255);
    if (enableWide) emitSolidQuad(x + 24, y + 66, x + 34, y + 66, x + 34, y + 74, x + 24, y + 74, 255, 120, 80, 255);
    if (enableTiled) emitSolidQuad(x + 40, y + 66, x + 50, y + 66, x + 50, y + 74, x + 40, y + 74, 255, 255, 80, 255);
    if (enableCrossSlice) emitSolidQuad(x + 56, y + 66, x + 66, y + 66, x + 66, y + 74, x + 56, y + 74, 255, 80, 255, 255);
}

static void drawFrame(void) {
    uint64_t t0 = nowUs();

    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    setOrthoPixel();
    GX_SetZMode(GX_FALSE, GX_LEQUAL, GX_FALSE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

    // Clear
    emitSolidQuad(0, 0, (float)rmode->fbWidth, 0,
                  (float)rmode->fbWidth, (float)rmode->efbHeight,
                  0, (float)rmode->efbHeight, 12, 12, 18, 255);

    if (enableTiled) {
        drawSpriteTiled(&atlases[0], 0, 0, 64, 64, 0, 0, -camX, 400, 1, 1, 900, 200);
    }

    // Explicit cross-slice probe (tall atlas, straddling y=1024)
    if (enableCrossSlice && enableTall) {
        float x0, y0, x1, y1, x2, y2, x3, y3;
        worldToScreen(120, 180, &x0, &y0);
        worldToScreen(120 + 96, 180, &x1, &y1);
        worldToScreen(120 + 96, 180 + 72, &x2, &y2);
        worldToScreen(120, 180 + 72, &x3, &y3);
        drawAtlasRect(&atlases[1], 0, 1000, 96, 72,
                      x0, y0, x1, y1, x2, y2, x3, y3, 255, 255, 0, 255);
    }

    for (int i = 0; i < spriteCount; i++) {
        if (!enableTall && sprites[i].atlasId == 1) continue;
        if (!enableWide && sprites[i].atlasId == 2) continue;
        drawSprite(&sprites[i]);
    }

    // Part-draw sample from wide atlas (original coords)
    if (enableWide) {
        float x0, y0, x1, y1, x2, y2, x3, y3;
        worldToScreen(500, 80, &x0, &y0);
        worldToScreen(500 + 64, 80, &x1, &y1);
        worldToScreen(500 + 64, 80 + 64, &x2, &y2);
        worldToScreen(500, 80 + 64, &x3, &y3);
        drawAtlasRect(&atlases[2], 256, 256, 128, 128,
                      x0, y0, x1, y1, x2, y2, x3, y3, 255, 200, 200, 255);
    }

    drawHud();

    GX_DrawDone();
    uint64_t t1 = nowUs();
    lastDrawMs = (float)(t1 - t0) / 1000.0f;

    GXColor clear = { 0, 0, 0, 255 };
    GX_SetCopyClear(clear, GX_MAX_Z24);
    GX_CopyDisp(xfb[fb], GX_TRUE);
    VIDEO_SetNextFramebuffer(xfb[fb]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    fb ^= 1;

    float frameMs = lastDrawMs; // approx; vsync dominates wall time
    float instant = frameMs > 0.01f ? (1000.0f / fmaxf(frameMs, 1.0f)) : 0.0f;
    // Prefer wall-clock FPS against vsync period ~16.7ms for display.
    // Recompute using draw+present wait is awkward; keep EMA of draw cost in HUD bars.
    (void)instant;
    float wallFps = 1000.0f / fmaxf(lastDrawMs + 0.01f, 1.0f);
    // Cap visual expectation: if draw << 16ms, real FPS is ~60/30 from vsync.
    if (lastDrawMs < 16.0f) wallFps = 60.0f;
    else wallFps = 1000.0f / lastDrawMs;
    if (fpsEma <= 0.0f) fpsEma = wallFps;
    else fpsEma = fpsEma * 0.9f + wallFps * 0.1f;
}

static void handleInput(void) {
    WPAD_ScanPads();
    u32 down = WPAD_ButtonsDown(0);
    if (down & WPAD_BUTTON_HOME) running = false;
    if (down & WPAD_BUTTON_LEFT) {
        if (spriteCount > 16) spriteCount /= 4;
        if (spriteCount < 16) spriteCount = 16;
    }
    if (down & WPAD_BUTTON_RIGHT) {
        if (spriteCount < DEMO_SPRITES_MAX) spriteCount *= 4;
        if (spriteCount > DEMO_SPRITES_MAX) spriteCount = DEMO_SPRITES_MAX;
    }
    if (down & WPAD_BUTTON_UP) scrollSpeed *= 1.5f;
    if (down & WPAD_BUTTON_DOWN) scrollSpeed *= 0.75f;
    if (down & WPAD_BUTTON_A) { enableTall = !enableTall; enableWide = !enableWide; }
    if (down & WPAD_BUTTON_B) enableTiled = !enableTiled;
    if (down & WPAD_BUTTON_1) enableCrossSlice = !enableCrossSlice;
}

static void initGx(void) {
    VIDEO_Init();
    WPAD_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb[0]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    fifoBuffer = MEM_K0_TO_K1(memalign(32, GX_FIFO_SIZE));
    memset(fifoBuffer, 0, GX_FIFO_SIZE);
    GX_Init(fifoBuffer, GX_FIFO_SIZE);
    GX_SetCopyClear((GXColor){0, 0, 0, 255}, GX_MAX_Z24);
    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
    GX_SetDispCopyYScale((f32)rmode->xfbHeight / (f32)rmode->efbHeight);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopyDst(rmode->fbWidth, rmode->xfbHeight);
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
    GX_SetFieldMode(rmode->field_rendering,
                    ((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
    if (rmode->aa) GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
    else GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_CopyDisp(xfb[fb], GX_TRUE);
    GX_SetDispCopyGamma(GX_GM_1_0);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    SYS_STDIO_Report(true);
    printf("wii-gx-demo starting\n");

    initGx();
    if (!createAtlases()) {
        printf("Atlas creation failed\n");
        while (1) VIDEO_WaitVSync();
    }
    seedSprites();
    printf("Controls: D-pad sprites/scroll, A tall/wide, B tiled, 1 cross-slice, HOME quit\n");

    while (running) {
        handleInput();
        updateSprites();
        drawFrame();
        // Periodic OSReport so Dolphin log proves which build is running.
        static int frames = 0;
        if ((++frames % 60) == 0) {
            printf("demo fps~%.1f draw=%.2fms sprites=%d tall=%d wide=%d tiled=%d cross=%d\n",
                   fpsEma, lastDrawMs, spriteCount,
                   (int)enableTall, (int)enableWide, (int)enableTiled, (int)enableCrossSlice);
        }
    }
    return 0;
}
