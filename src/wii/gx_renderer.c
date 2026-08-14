#include "gx_renderer.h"
#include "matrix_math.h"
#include "text_utils.h"
#include "utils.h"
#include "data_win.h"
#include "image_decoder.h"
#include "runner.h"
#include "stdio_compat.h"
#include "math_compat.h"
#include "string_compat.h"
#include "../gettime.h"

#include <gccore.h>
#include <ogc/gx.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ===[ Internal struct ]===

typedef struct {
    GXTexObj obj;
    uint8_t* data;
    int32_t y;
    int32_t height;
} GxTextureSlice;

typedef struct {
    Renderer base;
    GXRModeObj* rmode;
    void* xfb[2];
    u32* fbIndex;
    int32_t gameW, gameH, windowW, windowH;
    int32_t portX, portY, portW, portH;
    Matrix4f wvp;
    bool blendEnable;
    int32_t blendMode;
    BlendFactors blendFactors;
    bool alphaTestEnable;
    uint8_t alphaTestRef;
    bool colorWrite[4];
    uint32_t textureCount;
    GxTextureSlice** texSlices;
    int32_t* texSliceCounts;
    int32_t* texW;
    int32_t* texH;
    int32_t* texScale; // 1 = native; 2+ = nearest downscale applied on upload
    bool* texLoaded;
    uint64_t* texLastUsed;
    uint64_t* texTriedFrame;  // last frame we attempted a load (within-frame dedupe)
    uint64_t* texRetryAfter;  // earliest frame allowed to retry after transient fail
    uint8_t* texFailCount;    // exponential backoff for OOM / decode thrash
    uint8_t* texWtl1Next;     // 0xFF idle; else this page still has wanted slices to decode
    uint32_t* texSliceWanted; // sticky: slices a draw has asked for
    uint32_t* texSliceTouched; // this frame only — eviction must not dump these
    bool* texWanted;          // draw missed this page — pump after present
    uint64_t frameCounter;
    // Per-frame decode budget — cold TXTR loads on the draw thread hitch both
    // movement (1 tick/frame) and audio (stream refill runs after draw).
    uint32_t texColdLoadsFrame;
    uint32_t texDeferredFrame;
    uint32_t texEvictsFrame;
    uint64_t texDecodeNsFrame;
    int32_t boundPageId;      // last GX_LoadTexObj page (-1 = none)
    int32_t boundSliceIndex;  // last loaded slice within that page
    GXTexObj whiteTex;
    uint8_t* whiteTexData;
    int32_t appSurfW, appSurfH;
} GxRendererImpl;

// Amortize SD+decode+upload across frames. WTL1 pages decode ONE tile per frame
// (~1024² RGBA peak) — a full 4-tile page used to hitch ~4× as hard in one go.
#define GX_TEX_COLD_LOADS_PER_FRAME 1u
#define GX_TEX_DECODE_BUDGET_NS (6ull * 1000ull * 1000ull)
#define GX_TEX_WTL1_IDLE 0xFFu

static bool pageHasPendingSlices(const GxRendererImpl* gx, uint32_t pageId);
static void syncWtl1Progress(GxRendererImpl* gx, uint32_t pageId);

// ===[ Small helpers ]===

static inline uint8_t alphaToU8(float a) {
    if (a <= 0.0f) return 0;
    if (a >= 1.0f) return 255;
    return (uint8_t)(a * 255.0f + 0.5f);
}

// gx->wvp maps game/world space directly to EFB pixels (not NDC).
static inline void gameToScreen(const GxRendererImpl* gx, float wx, float wy, float* sx, float* sy) {
    Matrix4f_transformPoint(&gx->wvp, wx, wy, sx, sy);
}

// Bake OpenGL-style clip (Y-up NDC, room top → cy=+1) into EFB pixels (Y-down).
static void setScreenWvpFromClip(GxRendererImpl* gx, const Matrix4f* clipWvp) {
    float pw = (float)gx->portW;
    float ph = (float)gx->portH;
    float px = (float)gx->portX;
    float py = (float)gx->portY;
    Matrix4f ndcToScreen;
    Matrix4f_identity(&ndcToScreen);
    ndcToScreen.m[0]  =  0.5f * pw;
    ndcToScreen.m[5]  = -0.5f * ph; // cy=+1 → top of port
    ndcToScreen.m[12] =  0.5f * pw + px;
    ndcToScreen.m[13] =  0.5f * ph + py;
    Matrix4f_multiply(&gx->wvp, &ndcToScreen, clipWvp);
}

// Load GX pass-through matrices so submitted EFB-pixel positions render correctly.
// Match libogc gxSprites: ortho in pixel space + modelview pushed to z=-5 so Z=0 verts aren't near-clipped.
// Viewport stays full-EFB; port clipping is done with scissor only (avoids double-mapping port+ortho).
static void setGXPassthrough(GxRendererImpl* gx) {
    Mtx44 proj;
    f32 bottom = (f32)gx->rmode->efbHeight;
    f32 right  = (f32)gx->rmode->fbWidth;
    if (bottom < 1.0f) bottom = 1.0f;
    if (right < 1.0f) right = 1.0f;
    guOrtho(proj, 0.0f, bottom, 0.0f, right, 0.0f, 300.0f);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    Mtx mv;
    guMtxIdentity(mv);
    guMtxTransApply(mv, mv, 0.0f, 0.0f, -5.0f);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
}

// Map a GML blend-factor constant to the GX equivalent.
static u8 gmlBlendToGX(int32_t f) {
    switch (f) {
        case bm_zero:          return GX_BL_ZERO;
        case bm_one:           return GX_BL_ONE;
        case bm_src_color:     return GX_BL_SRCCLR;
        case bm_inv_src_color: return GX_BL_INVSRCCLR;
        case bm_src_alpha:     return GX_BL_SRCALPHA;
        case bm_inv_src_alpha: return GX_BL_INVSRCALPHA;
        case bm_dest_alpha:    return GX_BL_DSTALPHA;
        case bm_inv_dest_alpha:return GX_BL_INVDSTALPHA;
        case bm_dest_color:    return GX_BL_DSTCLR;
        case bm_inv_dest_color:return GX_BL_INVDSTCLR;
        default:               return GX_BL_ONE;
    }
}

static void applyBlend(GxRendererImpl* gx) {
    if (!gx->blendEnable) {
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
        return;
    }
    switch (gx->blendMode) {
        case bm_normal:
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
            break;
        case bm_add:
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
            break;
        case bm_subtract:
        case bm_reverse_subtract:
            GX_SetBlendMode(GX_BM_SUBTRACT, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
            break;
        case bm_max:
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
            break;
        case bm_complex:
            GX_SetBlendMode(GX_BM_BLEND,
                gmlBlendToGX(gx->blendFactors.src),
                gmlBlendToGX(gx->blendFactors.dst),
                GX_LO_CLEAR);
            break;
        default:
            GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
            break;
    }
}

static inline void setTEVTextured(void) {
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
}

static inline void setTEVSolid(void) {
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_DISABLE, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
}

// ===[ Texture loading ]===

// RGB5A3 halves residency vs RGBA8 so room working sets (BG + Frisk + Toriel + FX)
// fit under Wii heap pressure. Decode still peaks as temporary RGBA.
#define GX_TEX_BPP 2u
#define GX_TEX_RESIDENT_BUDGET (40ull * 1024ull * 1024ull)

static inline uint16_t packRgb5a3(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    // Punch-through: 3-bit RGB5A3 alpha made UI boxes / writer text look see-through.
    if (a >= 32) {
        return (uint16_t)(0x8000u | ((uint16_t)(r >> 3) << 10) | ((uint16_t)(g >> 3) << 5) | (uint16_t)(b >> 3));
    }
    return 0;
}

static void freeTexPage(GxRendererImpl* gx, uint32_t pageId) {
    if (!gx->texSlices || pageId >= gx->textureCount) return;
    GxTextureSlice* slices = gx->texSlices[pageId];
    if (slices) {
        for (int32_t i = 0; i < gx->texSliceCounts[pageId]; i++) {
            free(slices[i].data);
            slices[i].data = NULL;
        }
        free(slices);
    }
    gx->texSlices[pageId] = NULL;
    gx->texSliceCounts[pageId] = 0;
    gx->texW[pageId] = 0;
    gx->texH[pageId] = 0;
    gx->texScale[pageId] = 1;
    gx->texLoaded[pageId] = false;
    gx->texLastUsed[pageId] = 0;
    if (gx->texWtl1Next) gx->texWtl1Next[pageId] = GX_TEX_WTL1_IDLE;
    // Keep slice-wanted bits so the next pump restores only the visible tiles.
    if (gx->texSliceWanted && gx->texSliceWanted[pageId] && gx->texWanted) {
        gx->texWanted[pageId] = true;
    }
    if (gx->boundPageId == (int32_t)pageId) {
        gx->boundPageId = -1;
        gx->boundSliceIndex = -1;
    }
}

static uint64_t residentTexBytes(const GxRendererImpl* gx) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < gx->textureCount; i++) {
        if (!gx->texSlices || !gx->texSlices[i] || !gx->texW || gx->texW[i] == 0) continue;
        for (int32_t s = 0; s < gx->texSliceCounts[i]; s++) {
            if (gx->texSlices[i][s].data) {
                total += (uint64_t)gx->texW[i] * (uint64_t)gx->texSlices[i][s].height * (uint64_t)GX_TEX_BPP;
            }
        }
    }
    return total;
}

static bool evictLRUTexSlice(GxRendererImpl* gx, uint32_t excludePageId) {
    uint32_t bestPage = UINT32_MAX;
    int32_t bestSlice = -1;
    uint64_t bestScore = UINT64_MAX;

    for (uint32_t i = 0; i < gx->textureCount; i++) {
        if (i == excludePageId) continue;
        if (!gx->texSlices || !gx->texSlices[i] || !gx->texW || gx->texW[i] == 0) continue;
        uint32_t touched = gx->texSliceTouched ? gx->texSliceTouched[i] : 0;
        uint64_t used = gx->texLastUsed[i];
        for (int32_t s = 0; s < gx->texSliceCounts[i]; s++) {
            if (!gx->texSlices[i][s].data) continue;
            if (s < 32 && (touched & (1u << s))) continue;
            uint64_t score = used;
            if (used >= gx->frameCounter) score += (1ull << 60);
            if (score < bestScore) {
                bestScore = score;
                bestPage = i;
                bestSlice = s;
            }
        }
    }
    if (bestSlice < 0) return false;

    free(gx->texSlices[bestPage][bestSlice].data);
    gx->texSlices[bestPage][bestSlice].data = NULL;
    gx->texEvictsFrame++;
    if (gx->boundPageId == (int32_t)bestPage && gx->boundSliceIndex == bestSlice) {
        gx->boundPageId = -1;
        gx->boundSliceIndex = -1;
    }
    GX_InvalidateTexAll();
    return true;
}

static bool evictLRUTexPage(GxRendererImpl* gx, uint32_t excludePageId) {
    // Drop unused 1024-bands first. Dumping a whole atlas mid-attack caused
    // flicker (sprites vanish) + hitch (the same 2MB tiles streamed back in).
    if (evictLRUTexSlice(gx, excludePageId)) return true;

    uint32_t bestIdle = UINT32_MAX;
    uint64_t bestIdleUsed = UINT64_MAX;

    for (uint32_t i = 0; i < gx->textureCount; i++) {
        if (i == excludePageId) continue;
        if (!gx->texSlices || !gx->texSlices[i] || !gx->texW || gx->texW[i] == 0) continue;
        bool inProgress = gx->texWtl1Next && gx->texWtl1Next[i] != GX_TEX_WTL1_IDLE;
        if (inProgress) continue;
        uint64_t used = gx->texLastUsed[i];
        if (used >= gx->frameCounter) continue; // never evict what this frame still draws
        if (used < bestIdleUsed) {
            bestIdleUsed = used;
            bestIdle = i;
        }
    }
    if (bestIdle == UINT32_MAX) return false;
    logInfo("GxRenderer: Evicting TXTR page %u\n", bestIdle);
    freeTexPage(gx, bestIdle);
    gx->texEvictsFrame++;
    GX_InvalidateTexAll();
    return true;
}

static void markTexTransientFail(GxRendererImpl* gx, uint32_t pageId, const char* why) {
    uint8_t fails = gx->texFailCount[pageId];
    if (fails < 255) gx->texFailCount[pageId] = (uint8_t)(fails + 1);
    // 1,2,4,8,16,32… capped at ~2s @30fps so we don't soft-lock forever.
    uint32_t shift = fails < 5 ? fails : 5;
    uint64_t cool = 1ull << shift;
    if (cool > 60ull) cool = 60ull;
    gx->texRetryAfter[pageId] = gx->frameCounter + cool;
    // Rate-limit: first failure + every 8th thereafter.
    if (fails == 0 || (fails & 7u) == 0u) {
        logWarn("GxRenderer: %s (page %u, retry in %llu frames, fails=%u)\n",
            why, pageId, (unsigned long long)cool, (unsigned)gx->texFailCount[pageId]);
    }
}

static void ensureTexBudget(GxRendererImpl* gx, uint32_t excludePageId, uint64_t upcomingBytes) {
    while (residentTexBytes(gx) + upcomingBytes > GX_TEX_RESIDENT_BUDGET) {
        if (!evictLRUTexPage(gx, excludePageId)) break;
    }
}

static void* allocTexBytes(GxRendererImpl* gx, uint32_t excludePageId, size_t bytes) {
    for (;;) {
        void* ptr = memalign(32, bytes);
        if (ptr) return ptr;
        if (!evictLRUTexPage(gx, excludePageId)) return NULL;
    }
}

static void* allocHeapBytes(GxRendererImpl* gx, uint32_t excludePageId, size_t bytes) {
    for (;;) {
        void* ptr = malloc(bytes);
        if (ptr) return ptr;
        if (!evictLRUTexPage(gx, excludePageId)) return NULL;
    }
}

static bool peekPngSize(const uint8_t* blob, size_t blobSize, int* outW, int* outH) {
    if (blobSize < 24) return false;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (memcmp(blob, sig, 8) != 0) return false;
    // IHDR length(4) + type(4) + width/height
    uint32_t w = ((uint32_t)blob[16] << 24) | ((uint32_t)blob[17] << 16) | ((uint32_t)blob[18] << 8) | blob[19];
    uint32_t h = ((uint32_t)blob[20] << 24) | ((uint32_t)blob[21] << 16) | ((uint32_t)blob[22] << 8) | blob[23];
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return false;
    *outW = (int)w;
    *outH = (int)h;
    return true;
}

// WTL1: tiled PNG (Undertale face atlases). WTL2: pre-swizzled GX RGB5A3 tiles.
// magic "WTL1"/"WTL2" + u32 width,height,tileH,tileCount + tileCount×(u32 offset,u32 size) + tiles.
static bool parseWtlHeader(
    const uint8_t* hdr, int* outW, int* outH, uint32_t* outTileH, uint32_t* outTileCount, bool* outRawGx
) {
    if (!hdr) return false;
    if (hdr[0] != 'W' || hdr[1] != 'T' || hdr[2] != 'L' || (hdr[3] != '1' && hdr[3] != '2')) return false;
    uint32_t w = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    uint32_t h = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) | ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    uint32_t tileH = (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8) | ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
    uint32_t tileCount = (uint32_t)hdr[16] | ((uint32_t)hdr[17] << 8) | ((uint32_t)hdr[18] << 16) | ((uint32_t)hdr[19] << 24);
    if (w == 0 || h == 0 || w > 1024 || h > 8192 || tileCount == 0 || tileCount > 32) return false;
    if (tileH == 0 || tileH > 1024) tileH = 1024;
    *outW = (int)w;
    *outH = (int)h;
    if (outTileH) *outTileH = tileH;
    if (outTileCount) *outTileCount = tileCount;
    if (outRawGx) *outRawGx = (hdr[3] == '2');
    return true;
}

static void downsampleRgbaNNInPlace(uint8_t* px, int w, int h, int scale, int* outW, int* outH) {
    int dw = w / scale;
    int dh = h / scale;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            const uint8_t* s = px + (((y * scale) * w) + (x * scale)) * 4;
            uint8_t* d = px + (y * dw + x) * 4;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        }
    }
    *outW = dw;
    *outH = dh;
}

static void bindTexSlice(GxRendererImpl* gx, int32_t pageId, int32_t sliceIndex, GxTextureSlice* slice) {
    if (gx->boundPageId == pageId && gx->boundSliceIndex == sliceIndex) return;
    GX_LoadTexObj(&slice->obj, GX_TEXMAP0);
    gx->boundPageId = pageId;
    gx->boundSliceIndex = sliceIndex;
}

// Pack one vertical GX slice from tile-local RGBA (0..srcH rows, width srcW).
static bool uploadRgb5a3Slice(
    GxRendererImpl* gx, uint32_t pageId, GxTextureSlice* slice,
    const uint8_t* pixels, int srcW, int srcH, int pw
) {
    int phSlice = srcH;
    if (phSlice > 1024) phSlice = 1024;
    phSlice = (phSlice + 3) & ~3;
    if (phSlice > 1024) phSlice = 1024;
    slice->height = phSlice;

    size_t bufSize = (size_t)pw * (size_t)slice->height * GX_TEX_BPP;
    slice->data = (uint8_t*)allocTexBytes(gx, pageId, bufSize);
    if (!slice->data) return false;
    memset(slice->data, 0, bufSize);

    int tilesX = pw / 4;
    int tilesY = slice->height / 4;
    for (int ty = 0; ty < tilesY; ty++) {
        for (int tx = 0; tx < tilesX; tx++) {
            uint16_t* tile = (uint16_t*)(slice->data + (ty * tilesX + tx) * 32);
            for (int row = 0; row < 4; row++) {
                for (int col = 0; col < 4; col++) {
                    int px = tx * 4 + col;
                    int py = ty * 4 + row;
                    uint8_t R = 0, G = 0, B = 0, A = 0;
                    if (px < srcW && py < srcH) {
                        const uint8_t* p = pixels + (py * srcW + px) * 4;
                        R = p[0]; G = p[1]; B = p[2]; A = p[3];
                    }
                    tile[row * 4 + col] = packRgb5a3(R, G, B, A);
                }
            }
        }
    }

    GX_InitTexObj(&slice->obj, slice->data, (u16)pw, (u16)slice->height,
                  GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&slice->obj, GX_NEAR, GX_NEAR);
    DCFlushRange(slice->data, bufSize);
    return true;
}

static bool finishTexPageLoad(
    GxRendererImpl* gx, uint32_t pageId, Texture* txtr,
    GxTextureSlice* slices, int sliceCount, int pw, int ph, int scale, int w, int h
) {
    if (!txtr->mapped && txtr->blobData) {
        free(txtr->blobData);
        txtr->blobData = nullptr;
    }
    GX_InvalidateTexAll();
    gx->boundPageId = -1;
    gx->boundSliceIndex = -1;
    gx->texSlices[pageId] = slices;
    gx->texSliceCounts[pageId] = sliceCount;
    gx->texW[pageId] = pw;
    gx->texH[pageId] = ph;
    gx->texScale[pageId] = scale;
    gx->texLoaded[pageId] = true;
    gx->texLastUsed[pageId] = gx->frameCounter;
    gx->texFailCount[pageId] = 0;
    gx->texRetryAfter[pageId] = 0;
    logInfo("GxRenderer: Loaded TXTR page %u (%dx%d, scale=%d, %d slice%s, resident=%.1fMB)\n",
        pageId, w, h, scale, sliceCount, sliceCount == 1 ? "" : "s",
        (double)residentTexBytes(gx) / (1024.0 * 1024.0));
    return true;
}

static void markSliceWanted(GxRendererImpl* gx, uint32_t pageId, int32_t sourceY, int32_t sourceH) {
    if (pageId >= gx->textureCount) return;
    int32_t scale = (gx->texScale && gx->texScale[pageId] > 0) ? gx->texScale[pageId] : 1;
    int32_t y0 = sourceY / scale;
    int32_t y1 = (sourceY + (sourceH > 0 ? sourceH : 1) - 1) / scale;
    if (y0 < 0) y0 = 0;
    if (y1 < y0) y1 = y0;
    uint32_t s0 = (uint32_t)(y0 / 1024);
    uint32_t s1 = (uint32_t)(y1 / 1024);
    if (s0 > 31u) s0 = 31u;
    if (s1 > 31u) s1 = 31u;
    if (gx->texSliceWanted) {
        for (uint32_t s = s0; s <= s1; s++) gx->texSliceWanted[pageId] |= (1u << s);
    }
    if (gx->texSliceTouched) {
        for (uint32_t s = s0; s <= s1; s++) gx->texSliceTouched[pageId] |= (1u << s);
    }
    if (gx->texWanted) gx->texWanted[pageId] = true;
    if (gx->texLastUsed && gx->texLastUsed[pageId] < gx->frameCounter) {
        gx->texLastUsed[pageId] = gx->frameCounter;
    }
    syncWtl1Progress(gx, pageId);
}

static bool pageHasPendingSlices(const GxRendererImpl* gx, uint32_t pageId) {
    uint32_t mask = gx->texSliceWanted ? gx->texSliceWanted[pageId] : 0;
    if (mask == 0) return false;
    if (!gx->texSlices || !gx->texSlices[pageId] || gx->texSliceCounts[pageId] <= 0) return true;
    int n = gx->texSliceCounts[pageId];
    if (n > 32) n = 32;
    for (int s = 0; s < n; s++) {
        if ((mask & (1u << s)) && !gx->texSlices[pageId][s].data) return true;
    }
    return false;
}

static int32_t firstPendingSlice(const GxRendererImpl* gx, uint32_t pageId, int tileCount) {
    uint32_t mask = gx->texSliceWanted ? gx->texSliceWanted[pageId] : 0;
    if (mask == 0) return -1;
    GxTextureSlice* slices = gx->texSlices[pageId];
    int n = tileCount < 32 ? tileCount : 32;
    for (int i = 0; i < n; i++) {
        if (!(mask & (1u << i))) continue;
        if (slices && slices[i].data) continue;
        return i;
    }
    return -1;
}

static void syncWtl1Progress(GxRendererImpl* gx, uint32_t pageId) {
    if (!gx->texWtl1Next) return;
    gx->texWtl1Next[pageId] = pageHasPendingSlices(gx, pageId) ? 0 : GX_TEX_WTL1_IDLE;
}

static bool wtl1AllTilesResident(const GxRendererImpl* gx, uint32_t pageId, int tileCount) {
    if (!gx->texSlices || !gx->texSlices[pageId]) return false;
    for (int i = 0; i < tileCount; i++) {
        if (!gx->texSlices[pageId][i].data) return false;
    }
    return true;
}

static bool loadWtlTexPage(GxRendererImpl* gx, uint32_t pageId, Texture* txtr, bool gm2022_5) {
    DataWin* dw = gx->base.dataWin;
    uint8_t hdr[20];
    if (!DataWin_readTxtr(dw, pageId, 0, 20, hdr)) {
        markTexTransientFail(gx, pageId, "Failed to read WTL header");
        return false;
    }
    int w = 0, h = 0;
    uint32_t tileH = 1024, tileCount = 0;
    bool rawGx = false;
    if (!parseWtlHeader(hdr, &w, &h, &tileH, &tileCount, &rawGx)) {
        markTexTransientFail(gx, pageId, "Invalid WTL TXTR");
        return false;
    }

    int pw = (w + 3) & ~3;
    int ph = (h + 3) & ~3;
    if (pw > 1024) {
        gx->texLoaded[pageId] = true;
        gx->texWtl1Next[pageId] = GX_TEX_WTL1_IDLE;
        logWarn("GxRenderer: WTL page %u too wide (%d)\n", pageId, pw);
        return false;
    }

    if (!gx->texSlices[pageId]) {
        GxTextureSlice* slices = (GxTextureSlice*)allocHeapBytes(
            gx, pageId, (size_t)tileCount * sizeof(GxTextureSlice));
        if (!slices) {
            markTexTransientFail(gx, pageId, "WTL slice table alloc failed");
            return false;
        }
        memset(slices, 0, (size_t)tileCount * sizeof(GxTextureSlice));
        gx->texSlices[pageId] = slices;
        gx->texSliceCounts[pageId] = (int32_t)tileCount;
        gx->texW[pageId] = pw;
        gx->texH[pageId] = ph;
        gx->texScale[pageId] = 1;
        gx->texLoaded[pageId] = true;
        gx->texLastUsed[pageId] = gx->frameCounter;
        if (gx->texSliceWanted && tileCount < 32u) {
            gx->texSliceWanted[pageId] &= (tileCount == 0u) ? 0u : ((1u << tileCount) - 1u);
        }
    }

    GxTextureSlice* slices = gx->texSlices[pageId];
    int32_t i = firstPendingSlice(gx, pageId, (int)tileCount);
    if (i < 0) {
        syncWtl1Progress(gx, pageId);
        if (wtl1AllTilesResident(gx, pageId, (int)tileCount)) {
            return finishTexPageLoad(gx, pageId, txtr, slices, (int)tileCount, pw, ph, 1, w, h);
        }
        return true;
    }

    uint8_t ent[8];
    if (!DataWin_readTxtr(dw, pageId, 20u + (uint32_t)i * 8u, 8, ent)) {
        markTexTransientFail(gx, pageId, "WTL tile index read failed");
        return false;
    }
    uint32_t off = (uint32_t)ent[0] | ((uint32_t)ent[1] << 8) |
                   ((uint32_t)ent[2] << 16) | ((uint32_t)ent[3] << 24);
    uint32_t sz  = (uint32_t)ent[4] | ((uint32_t)ent[5] << 8) |
                   ((uint32_t)ent[6] << 16) | ((uint32_t)ent[7] << 24);
    if (sz == 0) {
        if (gx->texSliceWanted) gx->texSliceWanted[pageId] &= ~(1u << i);
        syncWtl1Progress(gx, pageId);
        markTexTransientFail(gx, pageId, "WTL tile empty");
        return false;
    }

    ensureTexBudget(gx, pageId, (uint64_t)pw * (uint64_t)tileH * (uint64_t)GX_TEX_BPP);

    GxTextureSlice* slice = &slices[i];
    slice->y = (int32_t)((uint32_t)i * tileH);

    if (rawGx) {
        int rem = ph - slice->y;
        int phSlice = rem > (int)tileH ? (int)tileH : rem;
        if (phSlice < 1) phSlice = 1;
        phSlice = (phSlice + 3) & ~3;
        if (phSlice > 1024) phSlice = 1024;
        slice->height = phSlice;
        size_t bufSize = (size_t)pw * (size_t)slice->height * GX_TEX_BPP;
        if ((size_t)sz != bufSize) {
            markTexTransientFail(gx, pageId, "WTL2 tile size mismatch");
            return false;
        }
        slice->data = (uint8_t*)allocTexBytes(gx, pageId, bufSize);
        if (!slice->data) {
            markTexTransientFail(gx, pageId, "memalign failed for WTL2 slice");
            return false;
        }
        if (!DataWin_readTxtr(dw, pageId, off, sz, slice->data)) {
            free(slice->data);
            slice->data = NULL;
            markTexTransientFail(gx, pageId, "WTL2 tile read failed");
            return false;
        }
        GX_InitTexObj(&slice->obj, slice->data, (u16)pw, (u16)slice->height,
                      GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjFilterMode(&slice->obj, GX_NEAR, GX_NEAR);
        DCFlushRange(slice->data, bufSize);
    } else {
        uint8_t* png = (uint8_t*)allocHeapBytes(gx, pageId, sz);
        if (!png) {
            markTexTransientFail(gx, pageId, "WTL1 tile alloc failed");
            return false;
        }
        if (!DataWin_readTxtr(dw, pageId, off, sz, png)) {
            free(png);
            markTexTransientFail(gx, pageId, "WTL1 tile read failed");
            return false;
        }
        int tw = 0, th = 0;
        uint8_t* pixels = ImageDecoder_decodeToRgba(png, (size_t)sz, gm2022_5, &tw, &th);
        free(png);
        if (!pixels) {
            markTexTransientFail(gx, pageId, "Failed to decode WTL1 tile");
            return false;
        }
        if (!uploadRgb5a3Slice(gx, pageId, slice, pixels, tw, th, pw)) {
            ImageDecoder_freeRgba(pixels);
            slice->data = NULL;
            markTexTransientFail(gx, pageId, "memalign failed for WTL1 slice");
            return false;
        }
        ImageDecoder_freeRgba(pixels);
    }

    gx->texW[pageId] = pw;
    gx->texH[pageId] = ph;
    gx->texScale[pageId] = 1;
    gx->texLoaded[pageId] = true;
    gx->texLastUsed[pageId] = gx->frameCounter;
    gx->boundPageId = -1;
    gx->boundSliceIndex = -1;
    GX_InvalidateTexAll();
    syncWtl1Progress(gx, pageId);

    if (wtl1AllTilesResident(gx, pageId, (int)tileCount)) {
        return finishTexPageLoad(gx, pageId, txtr, slices, (int)tileCount, pw, ph, 1, w, h);
    }
    return true;
}

static bool advanceTexLoad(GxRendererImpl* gx, uint32_t pageId) {
    // Permanent logical failure (no blob / corrupt) keeps texLoaded set with texW==0.
    if (gx->texLoaded[pageId] && gx->texW[pageId] == 0) {
        if (gx->texWanted) gx->texWanted[pageId] = false;
        return false;
    }
    if (gx->texLoaded[pageId] && gx->texW[pageId] != 0) {
        bool pending = pageHasPendingSlices(gx, pageId);
        if (!pending) {
            if (gx->texWanted) gx->texWanted[pageId] = false;
            return true;
        }
        // Fall through to decode a wanted WTL1 slice.
    }
    if (gx->frameCounter < gx->texRetryAfter[pageId]) return false;

    uint64_t loadStartNs = nowNanos();

    DataWin* dw = gx->base.dataWin;
    Texture* txtr = &dw->txtr.textures[pageId];

    uint8_t mag[20];
    if (DataWin_readTxtr(dw, pageId, 0, 20, mag)) {
        int wtlW = 0, wtlH = 0;
        uint32_t wtlTiles = 0;
        if (parseWtlHeader(mag, &wtlW, &wtlH, NULL, &wtlTiles, NULL)) {
            bool okWtl = loadWtlTexPage(gx, pageId, txtr, DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0));
            gx->texDecodeNsFrame += nowNanos() - loadStartNs;
            gx->texColdLoadsFrame++;
            if (okWtl && gx->texWanted && !pageHasPendingSlices(gx, pageId)) {
                gx->texWanted[pageId] = false;
            }
            syncWtl1Progress(gx, pageId);
            return okWtl;
        }
    }

    DataWin_loadTxtrIfNeeded(dw, pageId);
    if (!txtr->blobData) {
        gx->texLoaded[pageId] = true; // permanent
        if (gx->texWanted) gx->texWanted[pageId] = false;
        gx->texDecodeNsFrame += nowNanos() - loadStartNs;
        return false;
    }

    bool gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);
    bool ok = false;

    int peekW = 0, peekH = 0;

    if (peekPngSize(txtr->blobData, (size_t)txtr->blobSize, &peekW, &peekH)) {
        int scale = 1;
        while (peekW / scale > 1024) scale *= 2;
        uint64_t uploadBytes = (uint64_t)((peekW / scale + 3) & ~3) *
                               (uint64_t)((peekH / scale + 3) & ~3) * (uint64_t)GX_TEX_BPP;
        ensureTexBudget(gx, pageId, uploadBytes);
    } else {
        ensureTexBudget(gx, pageId, 8ull * 1024ull * 1024ull);
    }

    int w = 0, h = 0;
    uint8_t* pixels = ImageDecoder_decodeToRgba(txtr->blobData, (size_t)txtr->blobSize, gm2022_5, &w, &h);
    if (!pixels) {
        gx->texDecodeNsFrame += nowNanos() - loadStartNs;
        gx->texColdLoadsFrame++;
        markTexTransientFail(gx, pageId, "Failed to decode TXTR");
        return false;
    }

    int scale = 1;
    while (w / scale > 1024) scale *= 2;
    if (scale > 1) {
        int dwScale = 0, dhScale = 0;
        downsampleRgbaNNInPlace(pixels, w, h, scale, &dwScale, &dhScale);
        w = dwScale;
        h = dhScale;
    }

    int pw = (w + 3) & ~3;
    int ph = (h + 3) & ~3;
    if (pw > 1024) {
        ImageDecoder_freeRgba(pixels);
        gx->texLoaded[pageId] = true;
        if (gx->texWanted) gx->texWanted[pageId] = false;
        gx->texDecodeNsFrame += nowNanos() - loadStartNs;
        gx->texColdLoadsFrame++;
        return false;
    }

    ensureTexBudget(gx, pageId, (uint64_t)pw * (uint64_t)ph * (uint64_t)GX_TEX_BPP);

    int sliceCount = (ph + 1023) / 1024;
    GxTextureSlice* slices = (GxTextureSlice*)allocHeapBytes(gx, pageId, (size_t)sliceCount * sizeof(GxTextureSlice));
    if (!slices) {
        ImageDecoder_freeRgba(pixels);
        gx->texDecodeNsFrame += nowNanos() - loadStartNs;
        gx->texColdLoadsFrame++;
        markTexTransientFail(gx, pageId, "slice table alloc failed for TXTR");
        return false;
    }
    memset(slices, 0, (size_t)sliceCount * sizeof(GxTextureSlice));

    for (int sliceIndex = 0; sliceIndex < sliceCount; sliceIndex++) {
        GxTextureSlice* slice = &slices[sliceIndex];
        slice->y = sliceIndex * 1024;
        int rem = ph - slice->y;
        int segH = rem > 1024 ? 1024 : rem;
        const uint8_t* rowBase = pixels + (size_t)slice->y * (size_t)w * 4;
        if (!uploadRgb5a3Slice(gx, pageId, slice, rowBase, w, segH, pw)) {
            for (int i = 0; i < sliceIndex; i++) free(slices[i].data);
            free(slices);
            ImageDecoder_freeRgba(pixels);
            gx->texDecodeNsFrame += nowNanos() - loadStartNs;
            gx->texColdLoadsFrame++;
            markTexTransientFail(gx, pageId, "memalign failed for TXTR slice");
            return false;
        }
    }

    ImageDecoder_freeRgba(pixels);
    ok = finishTexPageLoad(gx, pageId, txtr, slices, sliceCount, pw, ph, scale, w, h);
    gx->texDecodeNsFrame += nowNanos() - loadStartNs;
    gx->texColdLoadsFrame++;
    if (ok && gx->texWanted) gx->texWanted[pageId] = false;
    return ok;
}

static bool ensureTexLoaded(GxRendererImpl* gx, uint32_t pageId) {
    if (gx->texLoaded[pageId] && gx->texW[pageId] != 0) {
        gx->texLastUsed[pageId] = gx->frameCounter;
        return true;
    }

    // Permanent logical failure (no blob / corrupt) keeps texLoaded set with texW==0.
    if (gx->texLoaded[pageId] && gx->texW[pageId] == 0) return false;

    // Never decode on the draw thread — queue for post-present pump so Step/Draw
    // (and therefore movement + audio refill timing) stay hitch-free.
    if (gx->frameCounter < gx->texRetryAfter[pageId]) return false;
    if (gx->texWanted && !gx->texWanted[pageId]) {
        gx->texWanted[pageId] = true;
        gx->texDeferredFrame++;
    }
    return false;
}

static bool resolveTpag(GxRendererImpl* gx, int32_t tpagIndex,
                        TexturePageItem** outTpag, int32_t* outPageId) {
    DataWin* dw = gx->base.dataWin;
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return false;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (pageId < 0 || (uint32_t)pageId >= gx->textureCount) return false;
    markSliceWanted(gx, (uint32_t)pageId, tpag->sourceY, tpag->sourceHeight);
    if (!ensureTexLoaded(gx, (uint32_t)pageId)) return false;
    *outTpag = tpag;
    *outPageId = (int32_t)pageId;
    return true;
}

static inline void mapAtlasRect(const GxRendererImpl* gx, int32_t pageId,
                                int32_t x, int32_t y, int32_t w, int32_t h,
                                int32_t* mx, int32_t* my, int32_t* mw, int32_t* mh) {
    int32_t scale = gx->texScale[pageId];
    if (scale < 1) scale = 1;
    *mx = x / scale;
    *my = y / scale;
    *mw = w / scale;
    *mh = h / scale;
    if (*mw < 1 && w > 0) *mw = 1;
    if (*mh < 1 && h > 0) *mh = 1;
}

static void emitTexQuad(
    float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
    float u0, float v0, float u1, float v1,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a
);

// Draw a source rectangle that may span vertical GX slices. Destination is a screen-space
// quad (TL,TR,BR,BL). Vertically subdivides by bilinear lerp so rotated sprites still work.
static void emitTexturedAtlasRect(
    GxRendererImpl* gx, int32_t pageId,
    int32_t sourceX, int32_t sourceY, int32_t sourceW, int32_t sourceH,
    float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (sourceW <= 0 || sourceH <= 0) return;

    int32_t mx, my, mw, mh;
    mapAtlasRect(gx, pageId, sourceX, sourceY, sourceW, sourceH, &mx, &my, &mw, &mh);
    if (mw <= 0 || mh <= 0) return;
    if (mx < 0) { mw += mx; mx = 0; }
    if (my < 0) { mh += my; my = 0; }
    if (mx >= gx->texW[pageId] || my >= gx->texH[pageId]) return;
    if (mx + mw > gx->texW[pageId]) mw = gx->texW[pageId] - mx;
    if (my + mh > gx->texH[pageId]) mh = gx->texH[pageId] - my;
    if (mw <= 0 || mh <= 0) return;

    float tw = (float)gx->texW[pageId];
    int32_t yCursor = my;
    int32_t yEnd = my + mh;

    setTEVTextured();
    applyBlend(gx);

    while (yCursor < yEnd) {
        int32_t sliceIndex = yCursor / 1024;
        if (sliceIndex < 0 || sliceIndex >= gx->texSliceCounts[pageId]) break;
        GxTextureSlice* slice = &gx->texSlices[pageId][sliceIndex];
        if (!slice->data || slice->height <= 0) {
            // Tile still streaming in — request just this band, skip pixels this frame.
            if (sliceIndex >= 0 && sliceIndex < 32) {
                if (gx->texSliceWanted) gx->texSliceWanted[pageId] |= (1u << sliceIndex);
                if (gx->texSliceTouched) gx->texSliceTouched[pageId] |= (1u << sliceIndex);
            }
            if (gx->texWanted) gx->texWanted[pageId] = true;
            syncWtl1Progress(gx, (uint32_t)pageId);
            int32_t bandEnd = (sliceIndex + 1) * 1024;
            if (bandEnd <= yCursor) bandEnd = yCursor + 1;
            yCursor = bandEnd;
            continue;
        }
        int32_t sliceEnd = slice->y + slice->height;
        if (sliceIndex >= 0 && sliceIndex < 32 && gx->texSliceTouched) {
            gx->texSliceTouched[pageId] |= (1u << sliceIndex);
        }
        int32_t segH = yEnd < sliceEnd ? (yEnd - yCursor) : (sliceEnd - yCursor);
        if (segH <= 0) break;

        float t0 = (float)(yCursor - my) / (float)mh;
        float t1 = (float)(yCursor + segH - my) / (float)mh;

        // Bilerp top/bottom edges of the destination quad.
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

        bindTexSlice(gx, pageId, sliceIndex, slice);
        emitTexQuad(sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3, u0, v0, u1, v1, r, g, b, a);

        yCursor += segH;
    }
}

// ===[ Draw quad helpers ]===

// Emit a textured QUAD (TL, TR, BR, BL) with per-corner screen coords and uniform color.
static void emitTexQuad(
    float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
    float u0, float v0, float u1, float v1,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a
) {
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position2f32(x0, y0); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u0, v0);
        GX_Position2f32(x1, y1); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u1, v0);
        GX_Position2f32(x2, y2); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u1, v1);
        GX_Position2f32(x3, y3); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u0, v1);
    GX_End();
}

// Emit a solid QUAD with per-corner colors (no texture).
static void emitSolidQuad(
    float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
    uint8_t r0, uint8_t g0, uint8_t b0,
    uint8_t r1, uint8_t g1, uint8_t b1,
    uint8_t r2, uint8_t g2, uint8_t b2,
    uint8_t r3, uint8_t g3, uint8_t b3,
    uint8_t a
) {
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position2f32(x0, y0); GX_Color4u8(r0, g0, b0, a); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position2f32(x1, y1); GX_Color4u8(r1, g1, b1, a); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position2f32(x2, y2); GX_Color4u8(r2, g2, b2, a); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position2f32(x3, y3); GX_Color4u8(r3, g3, b3, a); GX_TexCoord2f32(0.0f, 0.0f);
    GX_End();
}

// Transform a local-space corner through a combined local→EFB-pixel matrix.
#define TRANSFORM_CORNER(mat, lx, ly, pW, pH, pX, pY, outSx, outSy) do { \
    (void)(pW); (void)(pH); (void)(pX); (void)(pY); \
    Matrix4f_transformPoint((mat), (lx), (ly), &(outSx), &(outSy)); \
} while (0)

// ===[ Vtable: lifecycle ]===

static void gxInit(Renderer* renderer, DataWin* dataWin) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    renderer->dataWin = dataWin;

    Matrix4f id;
    Matrix4f_identity(&id);
    for (int i = 0; i < MATRICES_MAX; i++) renderer->gmlMatrices[i] = id;
    gx->wvp = id;

    gx->textureCount = dataWin->txtr.count;
    gx->texSlices = (GxTextureSlice**)safeCalloc(gx->textureCount, sizeof(GxTextureSlice*));
    gx->texSliceCounts = (int32_t*)safeCalloc(gx->textureCount, sizeof(int32_t));
    gx->texW      = (int32_t*)safeCalloc(gx->textureCount, sizeof(int32_t));
    gx->texH      = (int32_t*)safeCalloc(gx->textureCount, sizeof(int32_t));
    gx->texScale  = (int32_t*)safeCalloc(gx->textureCount, sizeof(int32_t));
    gx->texLoaded = (bool*)safeCalloc(gx->textureCount, sizeof(bool));
    gx->texLastUsed = (uint64_t*)safeCalloc(gx->textureCount, sizeof(uint64_t));
    gx->texTriedFrame = (uint64_t*)safeCalloc(gx->textureCount, sizeof(uint64_t));
    gx->texRetryAfter = (uint64_t*)safeCalloc(gx->textureCount, sizeof(uint64_t));
    gx->texFailCount = (uint8_t*)safeCalloc(gx->textureCount, sizeof(uint8_t));
    gx->texWtl1Next = (uint8_t*)safeCalloc(gx->textureCount, sizeof(uint8_t));
    memset(gx->texWtl1Next, GX_TEX_WTL1_IDLE, gx->textureCount);
    gx->texSliceWanted = (uint32_t*)safeCalloc(gx->textureCount, sizeof(uint32_t));
    gx->texSliceTouched = (uint32_t*)safeCalloc(gx->textureCount, sizeof(uint32_t));
    gx->texWanted = (bool*)safeCalloc(gx->textureCount, sizeof(bool));
    gx->frameCounter = 1;
    gx->boundPageId = -1;
    gx->boundSliceIndex = -1;
    for (uint32_t i = 0; i < gx->textureCount; i++) gx->texScale[i] = 1;

    // White 4x4 RGBA8 tiled texture: AR plane all 0xFF, GB plane all 0xFF.
    gx->whiteTexData = (uint8_t*)memalign(32, 64);
    memset(gx->whiteTexData, 0xFF, 64);
    GX_InitTexObj(&gx->whiteTex, gx->whiteTexData, 4, 4,
                  GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    GX_InitTexObjFilterMode(&gx->whiteTex, GX_NEAR, GX_NEAR);
    DCFlushRange(gx->whiteTexData, 64);

    // Vertex descriptor: 2D F32 position, RGBA8 colour, 2D F32 texcoord.
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS,  GX_POS_XY,  GX_F32,  0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST,  GX_F32,  0);

    // Required for vertex colours / colourized textures (see libogc lesson19).
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
                   GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

    GX_SetNumTexGens(1);
    GX_SetNumTevStages(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    setTEVTextured();

    GX_SetZMode(GX_FALSE, GX_LEQUAL, GX_FALSE);
    GX_SetZCompLoc(GX_FALSE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);

    gx->blendEnable = true;
    gx->blendMode = bm_normal;
    gx->blendFactors.src = bm_src_alpha;
    gx->blendFactors.dst = bm_inv_src_alpha;
    gx->blendFactors.srcAlpha = bm_src_alpha;
    gx->blendFactors.dstAlpha = bm_inv_src_alpha;
    gx->alphaTestEnable = false;
    gx->alphaTestRef = 0;
    gx->colorWrite[0] = gx->colorWrite[1] = gx->colorWrite[2] = gx->colorWrite[3] = true;
    applyBlend(gx);

    setGXPassthrough(gx);

    logInfo("GxRenderer: Initialized (%u texture pages)\n", gx->textureCount);
}

static void gxDestroy(Renderer* renderer) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    for (uint32_t i = 0; i < gx->textureCount; i++) {
        freeTexPage(gx, i);
    }
    free(gx->texSlices);
    free(gx->texSliceCounts);
    free(gx->texW);
    free(gx->texH);
    free(gx->texScale);
    free(gx->texLoaded);
    free(gx->texLastUsed);
    free(gx->texTriedFrame);
    free(gx->texRetryAfter);
    free(gx->texFailCount);
    free(gx->texWtl1Next);
    free(gx->texSliceWanted);
    free(gx->texSliceTouched);
    free(gx->texWanted);
    free(gx->whiteTexData);
    free(gx);
}

// ===[ Vtable: frame ]===

static void gxBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    gx->frameCounter++;
    if (gx->frameCounter == 0) gx->frameCounter = 1;
    // Decode counters are owned by pumpTexLoads (post-present). Only reset
    // draw-time "wanted" deferrals here so the overlay can still show last pump.
    gx->texDeferredFrame = 0;
    gx->boundPageId = -1;
    gx->boundSliceIndex = -1;
    if (gx->texSliceTouched) {
        memset(gx->texSliceTouched, 0, gx->textureCount * sizeof(uint32_t));
    }
    gx->gameW   = gameW;
    gx->gameH   = gameH;
    gx->windowW = windowW;
    gx->windowH = windowH;
    gx->portX = 0;
    gx->portY = 0;
    gx->portW = (int32_t)gx->rmode->fbWidth;
    gx->portH = (int32_t)gx->rmode->efbHeight;
    renderer->CPortX = 0;
    renderer->CPortY = 0;
    renderer->CPortW = gx->portW;
    renderer->CPortH = gx->portH;

    GX_SetViewport(0.0f, 0.0f, (f32)gx->rmode->fbWidth, (f32)gx->rmode->efbHeight, 0.0f, 1.0f);
    GX_SetScissor(0, 0, (u32)gx->rmode->fbWidth, (u32)gx->rmode->efbHeight);
    GX_InvVtxCache();
    setGXPassthrough(gx);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
}

static void gxEndFrameInit(MAYBE_UNUSED Renderer* renderer) {}
static void gxEndFrameEnd(MAYBE_UNUSED Renderer* renderer) {}

// ===[ Vtable: view ]===

static void gxBeginView(Renderer* renderer,
    int32_t viewX, int32_t viewY,
    int32_t viewW, int32_t viewH,
    int32_t portX, int32_t portY, int32_t portW, int32_t portH,
    float viewAngle)
{
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    gx->portX = portX;
    gx->portY = portY;
    gx->portW = portW;
    gx->portH = portH;
    renderer->CPortX = portX;
    renderer->CPortY = portY;
    renderer->CPortW = portW;
    renderer->CPortH = portH;

    // Keep GX viewport at full EFB (matches guOrtho). Port is applied in wvp + scissor.
    GX_SetViewport(0.0f, 0.0f, (f32)gx->rmode->fbWidth, (f32)gx->rmode->efbHeight, 0.0f, 1.0f);
    GX_SetScissor((u32)portX, (u32)portY, (u32)portW, (u32)portH);

    int32_t viewCurrent = renderer->runner->viewsEnabled ? renderer->runner->viewCurrent : 0;
    renderer->cameraCurrent = renderer->runner->views[viewCurrent].cameraId;
    GMLCamera* cam = Runner_getCameraById(renderer->runner, renderer->cameraCurrent);

    // Keep the runner's expanded integer rectangle, but restore the camera's fractional
    // origin so subpixel camera movement is not snapped away.
    Matrix4f viewMatrix;
    Matrix4f_identity(&viewMatrix);
    Matrix4f projMatrix;
    float fracX = (float)viewX + ((float)cam->viewX - truncf((float)cam->viewX));
    float fracY = (float)viewY + ((float)cam->viewY - truncf((float)cam->viewY));
    Matrix4f_viewProjection(&projMatrix, fracX, fracY, (float)viewW, (float)viewH, viewAngle);
    cam->viewMatrix = viewMatrix;
    cam->projectionMatrix = projMatrix;
    renderer->vtable->applyProjection(renderer, &viewMatrix, &projMatrix);
}

static void gxEndView(Renderer* renderer) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    GX_SetScissor(0, 0, (u32)gx->rmode->fbWidth, (u32)gx->rmode->efbHeight);
}

static void gxApplyProjection(Renderer* renderer, const Matrix4f* viewMatrix, const Matrix4f* projectionMatrix) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;

    Matrix4f world = renderer->gmlMatrices[MATRIX_WORLD];
    Matrix4f worldView;
    Matrix4f_multiply(&worldView, viewMatrix, &world);
    Matrix4f clipWvp;
    Matrix4f_multiply(&clipWvp, projectionMatrix, &worldView);

    renderer->gmlMatrices[MATRIX_VIEW]                = *viewMatrix;
    renderer->gmlMatrices[MATRIX_PROJECTION]          = *projectionMatrix;
    renderer->gmlMatrices[MATRIX_WORLD_VIEW]          = worldView;
    renderer->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = clipWvp;

    // Draw path uses pixel-space positions; bake NDC→EFB here (matches Runner mouse ndcY mapping).
    setScreenWvpFromClip(gx, &clipWvp);

    setGXPassthrough(gx);
}

// ===[ Vtable: GUI ]===

static void gxBeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH,
    int32_t portX, int32_t portY, int32_t portW, int32_t portH,
    MAYBE_UNUSED int32_t targetSurfaceId)
{
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    gx->portX = portX;
    gx->portY = portY;
    gx->portW = portW;
    gx->portH = portH;
    renderer->CPortX = portX;
    renderer->CPortY = portY;
    renderer->CPortW = portW;
    renderer->CPortH = portH;

    GX_SetViewport(0.0f, 0.0f, (f32)gx->rmode->fbWidth, (f32)gx->rmode->efbHeight, 0.0f, 1.0f);
    GX_SetScissor((u32)portX, (u32)portY, (u32)portW, (u32)portH);

    renderer->cameraCurrent = GUI_CAMERA;
    GMLCamera* camera = &renderer->runner->guiCamera;
    camera->allocated  = true;
    camera->viewX      = 0.0;
    camera->viewY      = 0.0;
    camera->viewWidth  = guiW;
    camera->viewHeight = guiH;
    camera->borderX    = 0;
    camera->borderY    = 0;
    camera->speedX     = 0;
    camera->speedY     = 0;
    camera->objectId   = -1;
    camera->viewAngle  = 0;

    Matrix4f projMatrix;
    Matrix4f_viewProjection(&projMatrix, 0.0f, 0.0f, (float)guiW, (float)guiH, 0.0f);
    Matrix4f viewMatrix;
    Matrix4f_identity(&viewMatrix);
    camera->viewMatrix       = viewMatrix;
    camera->projectionMatrix = projMatrix;

    renderer->vtable->applyProjection(renderer, &viewMatrix, &projMatrix);
}

static void gxSetGuiProjection(Renderer* renderer, int32_t guiW, int32_t guiH,
    MAYBE_UNUSED int32_t portW, MAYBE_UNUSED int32_t portH,
    MAYBE_UNUSED bool renderingToUserSurface)
{
    Matrix4f projMatrix;
    Matrix4f_viewProjection(&projMatrix, 0.0f, 0.0f, (float)guiW, (float)guiH, 0.0f);
    Matrix4f viewMatrix;
    Matrix4f_identity(&viewMatrix);
    renderer->vtable->applyProjection(renderer, &viewMatrix, &projMatrix);
}

static void gxEndGUI(Renderer* renderer) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    GX_SetScissor(0, 0, (u32)gx->rmode->fbWidth, (u32)gx->rmode->efbHeight);
}

// ===[ Vtable: clear ]===

static void gxClearScreen(Renderer* renderer, uint32_t color, float alpha) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;

    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = alphaToU8(alpha);

    float x0 = (float)gx->portX;
    float y0 = (float)gx->portY;
    float x1 = (float)(gx->portX + gx->portW);
    float y1 = (float)(gx->portY + gx->portH);

    setTEVSolid();
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);

    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position2f32(x0, y0); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position2f32(x1, y0); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position2f32(x1, y1); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0.0f, 0.0f);
        GX_Position2f32(x0, y1); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0.0f, 0.0f);
    GX_End();

    applyBlend(gx);
    setTEVTextured();
}

// ===[ Vtable: sprites ]===

static void gxDrawSprite(Renderer* renderer, int32_t tpagIndex,
    float x, float y, float originX, float originY,
    float xscale, float yscale, float angleDeg,
    uint32_t color, float alpha)
{
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    TexturePageItem* tpag;
    int32_t pageId;
    if (!resolveTpag(gx, tpagIndex, &tpag, &pageId)) return;

    float lx0 = (float)tpag->targetX - originX;
    float ly0 = (float)tpag->targetY - originY;
    float lx1 = lx0 + (float)tpag->targetWidth;
    float ly1 = ly0 + (float)tpag->targetHeight;

    float angleRad = -angleDeg * ((float)M_PI / 180.0f);
    Matrix4f sprT;
    Matrix4f_setTransform2D(&sprT, x, y, xscale, yscale, angleRad);
    Matrix4f combined;
    Matrix4f_multiply(&combined, &gx->wvp, &sprT);

    float sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3;
    TRANSFORM_CORNER(&combined, lx0, ly0, gx->portW, gx->portH, gx->portX, gx->portY, sx0, sy0);
    TRANSFORM_CORNER(&combined, lx1, ly0, gx->portW, gx->portH, gx->portX, gx->portY, sx1, sy1);
    TRANSFORM_CORNER(&combined, lx1, ly1, gx->portW, gx->portH, gx->portX, gx->portY, sx2, sy2);
    TRANSFORM_CORNER(&combined, lx0, ly1, gx->portW, gx->portH, gx->portX, gx->portY, sx3, sy3);

    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = alphaToU8(alpha);
    emitTexturedAtlasRect(gx, pageId,
        tpag->sourceX, tpag->sourceY, tpag->sourceWidth, tpag->sourceHeight,
        sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3, r, g, b, a);
}

static void gxDrawSpritePart(Renderer* renderer, int32_t tpagIndex,
    int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH,
    float x, float y, float xscale, float yscale, float angleDeg,
    float pivotX, float pivotY, uint32_t color, float alpha)
{
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    TexturePageItem* tpag;
    int32_t pageId;
    if (!resolveTpag(gx, tpagIndex, &tpag, &pageId)) return;

    float qx0 = x,                         qy0 = y;
    float qx1 = x + (float)srcW * xscale,  qy1 = y;
    float qx2 = x + (float)srcW * xscale,  qy2 = y + (float)srcH * yscale;
    float qx3 = x,                         qy3 = y + (float)srcH * yscale;

    if (angleDeg != 0.0f) {
        float angleRad = -angleDeg * ((float)M_PI / 180.0f);
        float cosA = cosf(angleRad);
        float sinA = sinf(angleRad);
        float dx, dy, rx, ry;
#define ROT_CORNER(px, py) do { dx=(px)-pivotX; dy=(py)-pivotY; rx=cosA*dx-sinA*dy+pivotX; ry=sinA*dx+cosA*dy+pivotY; (px)=rx; (py)=ry; } while(0)
        ROT_CORNER(qx0, qy0);
        ROT_CORNER(qx1, qy1);
        ROT_CORNER(qx2, qy2);
        ROT_CORNER(qx3, qy3);
#undef ROT_CORNER
    }

    float sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3;
    TRANSFORM_CORNER(&gx->wvp, qx0, qy0, gx->portW, gx->portH, gx->portX, gx->portY, sx0, sy0);
    TRANSFORM_CORNER(&gx->wvp, qx1, qy1, gx->portW, gx->portH, gx->portX, gx->portY, sx1, sy1);
    TRANSFORM_CORNER(&gx->wvp, qx2, qy2, gx->portW, gx->portH, gx->portX, gx->portY, sx2, sy2);
    TRANSFORM_CORNER(&gx->wvp, qx3, qy3, gx->portW, gx->portH, gx->portX, gx->portY, sx3, sy3);

    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = alphaToU8(alpha);
    emitTexturedAtlasRect(gx, pageId,
        (int32_t)tpag->sourceX + srcOffX, (int32_t)tpag->sourceY + srcOffY, srcW, srcH,
        sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3, r, g, b, a);
}

static void gxDrawSpritePos(Renderer* renderer, int32_t tpagIndex,
    float x1, float y1, float x2, float y2,
    float x3, float y3, float x4, float y4, float alpha)
{
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    TexturePageItem* tpag;
    int32_t pageId;
    if (!resolveTpag(gx, tpagIndex, &tpag, &pageId)) return;

    float sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3;
    TRANSFORM_CORNER(&gx->wvp, x1, y1, gx->portW, gx->portH, gx->portX, gx->portY, sx0, sy0);
    TRANSFORM_CORNER(&gx->wvp, x2, y2, gx->portW, gx->portH, gx->portX, gx->portY, sx1, sy1);
    TRANSFORM_CORNER(&gx->wvp, x3, y3, gx->portW, gx->portH, gx->portX, gx->portY, sx2, sy2);
    TRANSFORM_CORNER(&gx->wvp, x4, y4, gx->portW, gx->portH, gx->portX, gx->portY, sx3, sy3);

    emitTexturedAtlasRect(gx, pageId,
        tpag->sourceX, tpag->sourceY, tpag->sourceWidth, tpag->sourceHeight,
        sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3, 255, 255, 255, alphaToU8(alpha));
}

static void gxDrawSpriteTiled(Renderer* renderer, int32_t tpagIndex,
    float originX, float originY, float x, float y,
    float xscale, float yscale, bool tileX, bool tileY,
    float roomW, float roomH, uint32_t color, float alpha)
{
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    TexturePageItem* tpag;
    int32_t pageId;
    if (!resolveTpag(gx, tpagIndex, &tpag, &pageId)) return;

    float axs = fabsf(xscale);
    float ays = fabsf(yscale);
    float tileW = (float)tpag->boundingWidth  * axs;
    float tileH = (float)tpag->boundingHeight * ays;
    if (tileW < 0.5f || tileH < 0.5f) return;

    float lx0 = (float)tpag->targetX - originX;
    float ly0 = (float)tpag->targetY - originY;
    float quadOffX = originX * axs + xscale * lx0;
    float quadOffY = originY * ays + yscale * ly0;
    float quadW    = xscale * (float)tpag->targetWidth;
    float quadH    = yscale * (float)tpag->targetHeight;

    float gridX = x - originX * axs;
    float gridY = y - originY * ays;

    float startX, endX, startY, endY;
    if (tileX) {
        startX = fmodf(gridX, tileW);
        if (startX > 0.0f) startX -= tileW;
        endX = roomW;
    } else {
        startX = gridX; endX = gridX + tileW;
    }
    if (tileY) {
        startY = fmodf(gridY, tileH);
        if (startY > 0.0f) startY -= tileH;
        endY = roomH;
    } else {
        startY = gridY; endY = gridY + tileH;
    }

    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = alphaToU8(alpha);

    int32_t tilesY = (int32_t)((endY - startY) / tileH) + 1;
    int32_t tilesX = (int32_t)((endX - startX) / tileW) + 1;

    for (int32_t iy = 0; iy < tilesY; iy++) {
        float dy = startY + (float)iy * tileH;
        if (dy >= endY) break;
        float vy0 = dy + quadOffY;
        float vy1 = vy0 + quadH;
        for (int32_t ix = 0; ix < tilesX; ix++) {
            float dx = startX + (float)ix * tileW;
            if (dx >= endX) break;
            float vx0 = dx + quadOffX;
            float vx1 = vx0 + quadW;

            float sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3;
            TRANSFORM_CORNER(&gx->wvp, vx0, vy0, gx->portW, gx->portH, gx->portX, gx->portY, sx0, sy0);
            TRANSFORM_CORNER(&gx->wvp, vx1, vy0, gx->portW, gx->portH, gx->portX, gx->portY, sx1, sy1);
            TRANSFORM_CORNER(&gx->wvp, vx1, vy1, gx->portW, gx->portH, gx->portX, gx->portY, sx2, sy2);
            TRANSFORM_CORNER(&gx->wvp, vx0, vy1, gx->portW, gx->portH, gx->portX, gx->portY, sx3, sy3);
            emitTexturedAtlasRect(gx, pageId,
                tpag->sourceX, tpag->sourceY, tpag->sourceWidth, tpag->sourceHeight,
                sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3, r, g, b, a);
        }
    }
}

// ===[ Vtable: primitives ]===

static void gxDrawRectangle(Renderer* renderer,
    float x1, float y1, float x2, float y2,
    uint32_t color, float alpha, bool outline)
{
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = alphaToU8(alpha);

    float sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3;
    TRANSFORM_CORNER(&gx->wvp, x1, y1, gx->portW, gx->portH, gx->portX, gx->portY, sx0, sy0);
    TRANSFORM_CORNER(&gx->wvp, x2, y1, gx->portW, gx->portH, gx->portX, gx->portY, sx1, sy1);
    TRANSFORM_CORNER(&gx->wvp, x2, y2, gx->portW, gx->portH, gx->portX, gx->portY, sx2, sy2);
    TRANSFORM_CORNER(&gx->wvp, x1, y2, gx->portW, gx->portH, gx->portX, gx->portY, sx3, sy3);

    setTEVSolid();
    applyBlend(gx);

    if (outline) {
        GX_Begin(GX_LINESTRIP, GX_VTXFMT0, 5);
            GX_Position2f32(sx0, sy0); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0, 0);
            GX_Position2f32(sx1, sy1); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0, 0);
            GX_Position2f32(sx2, sy2); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0, 0);
            GX_Position2f32(sx3, sy3); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0, 0);
            GX_Position2f32(sx0, sy0); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0, 0);
        GX_End();
    } else {
        emitSolidQuad(sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3,
                      r, g, b, r, g, b, r, g, b, r, g, b, a);
    }
    setTEVTextured();
}

static void gxDrawRectangleColor(Renderer* renderer,
    float x1, float y1, float x2, float y2,
    uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4,
    float alpha, bool outline)
{
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    uint8_t a = alphaToU8(alpha);

    uint8_t r0 = BGR_R(color1), g0 = BGR_G(color1), b0 = BGR_B(color1);
    uint8_t r1 = BGR_R(color2), g1 = BGR_G(color2), b1 = BGR_B(color2);
    uint8_t r2 = BGR_R(color3), g2 = BGR_G(color3), b2 = BGR_B(color3);
    uint8_t r3 = BGR_R(color4), g3 = BGR_G(color4), b3 = BGR_B(color4);

    float sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3;
    TRANSFORM_CORNER(&gx->wvp, x1, y1, gx->portW, gx->portH, gx->portX, gx->portY, sx0, sy0);
    TRANSFORM_CORNER(&gx->wvp, x2, y1, gx->portW, gx->portH, gx->portX, gx->portY, sx1, sy1);
    TRANSFORM_CORNER(&gx->wvp, x2, y2, gx->portW, gx->portH, gx->portX, gx->portY, sx2, sy2);
    TRANSFORM_CORNER(&gx->wvp, x1, y2, gx->portW, gx->portH, gx->portX, gx->portY, sx3, sy3);

    setTEVSolid();
    applyBlend(gx);

    if (outline) {
        GX_Begin(GX_LINESTRIP, GX_VTXFMT0, 5);
            GX_Position2f32(sx0, sy0); GX_Color4u8(r0, g0, b0, a); GX_TexCoord2f32(0, 0);
            GX_Position2f32(sx1, sy1); GX_Color4u8(r1, g1, b1, a); GX_TexCoord2f32(0, 0);
            GX_Position2f32(sx2, sy2); GX_Color4u8(r2, g2, b2, a); GX_TexCoord2f32(0, 0);
            GX_Position2f32(sx3, sy3); GX_Color4u8(r3, g3, b3, a); GX_TexCoord2f32(0, 0);
            GX_Position2f32(sx0, sy0); GX_Color4u8(r0, g0, b0, a); GX_TexCoord2f32(0, 0);
        GX_End();
    } else {
        emitSolidQuad(sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3,
                      r0, g0, b0, r1, g1, b1, r2, g2, b2, r3, g3, b3, a);
    }
    setTEVTextured();
}

static void gxDrawLine(Renderer* renderer,
    float x1, float y1, float x2, float y2,
    float width, uint32_t color, float alpha)
{
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = alphaToU8(alpha);

    float sx1, sy1, sx2, sy2;
    TRANSFORM_CORNER(&gx->wvp, x1, y1, gx->portW, gx->portH, gx->portX, gx->portY, sx1, sy1);
    TRANSFORM_CORNER(&gx->wvp, x2, y2, gx->portW, gx->portH, gx->portX, gx->portY, sx2, sy2);

    setTEVSolid();
    applyBlend(gx);

    if (width <= 1.0f) {
        GX_Begin(GX_LINES, GX_VTXFMT0, 2);
            GX_Position2f32(sx1, sy1); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0, 0);
            GX_Position2f32(sx2, sy2); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(0, 0);
        GX_End();
    } else {
        float dx = sx2 - sx1;
        float dy = sy2 - sy1;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f) { setTEVTextured(); return; }
        float hw = width * 0.5f;
        float nx = -dy / len * hw;
        float ny =  dx / len * hw;
        emitSolidQuad(
            sx1 + nx, sy1 + ny, sx2 + nx, sy2 + ny,
            sx2 - nx, sy2 - ny, sx1 - nx, sy1 - ny,
            r, g, b, r, g, b, r, g, b, r, g, b, a);
    }
    setTEVTextured();
}

static void gxDrawLineColor(Renderer* renderer,
    float x1, float y1, float x2, float y2,
    float width, uint32_t color1, uint32_t color2, float alpha)
{
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    uint8_t r1 = BGR_R(color1), g1 = BGR_G(color1), b1 = BGR_B(color1);
    uint8_t r2 = BGR_R(color2), g2 = BGR_G(color2), b2 = BGR_B(color2);
    uint8_t a = alphaToU8(alpha);

    float sx1, sy1, sx2, sy2;
    TRANSFORM_CORNER(&gx->wvp, x1, y1, gx->portW, gx->portH, gx->portX, gx->portY, sx1, sy1);
    TRANSFORM_CORNER(&gx->wvp, x2, y2, gx->portW, gx->portH, gx->portX, gx->portY, sx2, sy2);

    setTEVSolid();
    applyBlend(gx);

    if (width <= 1.0f) {
        GX_Begin(GX_LINES, GX_VTXFMT0, 2);
            GX_Position2f32(sx1, sy1); GX_Color4u8(r1, g1, b1, a); GX_TexCoord2f32(0, 0);
            GX_Position2f32(sx2, sy2); GX_Color4u8(r2, g2, b2, a); GX_TexCoord2f32(0, 0);
        GX_End();
    } else {
        float dx = sx2 - sx1;
        float dy = sy2 - sy1;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f) { setTEVTextured(); return; }
        float hw = width * 0.5f;
        float nx = -dy / len * hw;
        float ny =  dx / len * hw;
        emitSolidQuad(
            sx1 + nx, sy1 + ny, sx2 + nx, sy2 + ny,
            sx2 - nx, sy2 - ny, sx1 - nx, sy1 - ny,
            r1, g1, b1, r2, g2, b2, r2, g2, b2, r1, g1, b1, a);
    }
    setTEVTextured();
}

static void gxDrawTriangle(Renderer* renderer,
    float x1, float y1, float x2, float y2, float x3, float y3,
    uint32_t color1, uint32_t color2, uint32_t color3, float alpha, bool outline)
{
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    uint8_t r1 = BGR_R(color1), g1v = BGR_G(color1), b1 = BGR_B(color1);
    uint8_t r2 = BGR_R(color2), g2v = BGR_G(color2), b2 = BGR_B(color2);
    uint8_t r3 = BGR_R(color3), g3v = BGR_G(color3), b3 = BGR_B(color3);
    uint8_t a = alphaToU8(alpha);

    float sx1, sy1, sx2, sy2, sx3, sy3;
    TRANSFORM_CORNER(&gx->wvp, x1, y1, gx->portW, gx->portH, gx->portX, gx->portY, sx1, sy1);
    TRANSFORM_CORNER(&gx->wvp, x2, y2, gx->portW, gx->portH, gx->portX, gx->portY, sx2, sy2);
    TRANSFORM_CORNER(&gx->wvp, x3, y3, gx->portW, gx->portH, gx->portX, gx->portY, sx3, sy3);

    setTEVSolid();
    applyBlend(gx);

    if (outline) {
        GX_Begin(GX_LINESTRIP, GX_VTXFMT0, 4);
            GX_Position2f32(sx1, sy1); GX_Color4u8(r1, g1v, b1, a); GX_TexCoord2f32(0, 0);
            GX_Position2f32(sx2, sy2); GX_Color4u8(r2, g2v, b2, a); GX_TexCoord2f32(0, 0);
            GX_Position2f32(sx3, sy3); GX_Color4u8(r3, g3v, b3, a); GX_TexCoord2f32(0, 0);
            GX_Position2f32(sx1, sy1); GX_Color4u8(r1, g1v, b1, a); GX_TexCoord2f32(0, 0);
        GX_End();
    } else {
        GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 3);
            GX_Position2f32(sx1, sy1); GX_Color4u8(r1, g1v, b1, a); GX_TexCoord2f32(0, 0);
            GX_Position2f32(sx2, sy2); GX_Color4u8(r2, g2v, b2, a); GX_TexCoord2f32(0, 0);
            GX_Position2f32(sx3, sy3); GX_Color4u8(r3, g3v, b3, a); GX_TexCoord2f32(0, 0);
        GX_End();
    }
    setTEVTextured();
}

// ===[ Text drawing ]===

typedef struct {
    Font* font;
    TexturePageItem* fontTpag;
    int32_t fontTpagIndex;
    int32_t pageId;
    Sprite* spriteFontSprite;
} GxFontState;

typedef struct {
    int32_t pageId;
    int32_t sourceX, sourceY, sourceW, sourceH;
    float localX0, localY0;
} GxGlyphDraw;

static bool gxResolveFontState(GxRendererImpl* gx, DataWin* dw, Font* font, GxFontState* state) {
    state->font = font;
    state->fontTpag = nullptr;
    state->fontTpagIndex = -1;
    state->pageId = -1;
    state->spriteFontSprite = nullptr;

    if (!font->isSpriteFont) {
        int32_t fontTpagIndex = font->tpagIndex;
        if (fontTpagIndex < 0) return false;
        TexturePageItem* tpag;
        int32_t pageId;
        if (!resolveTpag(gx, fontTpagIndex, &tpag, &pageId)) return false;
        state->fontTpagIndex = fontTpagIndex;
        state->fontTpag = tpag;
        state->pageId = pageId;
    } else if (font->spriteIndex >= 0 && dw->sprt.count > (uint32_t)font->spriteIndex) {
        state->spriteFontSprite = &dw->sprt.sprites[font->spriteIndex];
    } else {
        return false;
    }
    return true;
}

static bool gxResolveGlyph(GxRendererImpl* gx, MAYBE_UNUSED DataWin* dw, GxFontState* state,
                           FontGlyph* glyph, float cursorX, float cursorY,
                           GxGlyphDraw* out) {
    Font* font = state->font;
    int32_t pageId;
    int32_t sourceX, sourceY, sourceW, sourceH;

    if (font->isSpriteFont && state->spriteFontSprite != nullptr) {
        Sprite* sprite = state->spriteFontSprite;
        int32_t glyphIndex = (int32_t)(glyph - font->glyphs);
        if (glyphIndex < 0 || glyphIndex >= (int32_t)sprite->textureCount) return false;

        int32_t tpagIdx = sprite->tpagIndices[glyphIndex];
        TexturePageItem* glyphTpag;
        if (!resolveTpag(gx, tpagIdx, &glyphTpag, &pageId)) return false;

        sourceX = glyphTpag->sourceX;
        sourceY = glyphTpag->sourceY;
        sourceW = glyphTpag->sourceWidth;
        sourceH = glyphTpag->sourceHeight;

        out->localX0 = cursorX + (float)glyph->offset;
        out->localY0 = cursorY + (float)(int32_t)glyphTpag->targetY - (float)font->spriteOriginYAdjust;
    } else {
        pageId = state->pageId;
        sourceX = state->fontTpag->sourceX + glyph->sourceX;
        sourceY = state->fontTpag->sourceY + glyph->sourceY;
        sourceW = glyph->sourceWidth;
        sourceH = glyph->sourceHeight;

        out->localX0 = cursorX + (float)glyph->offset;
        out->localY0 = cursorY;
    }

    out->pageId = pageId;
    out->sourceX = sourceX;
    out->sourceY = sourceY;
    out->sourceW = sourceW;
    out->sourceH = sourceH;
    return true;
}

static void gxEmitGlyphQuad(GxRendererImpl* gx, const Matrix4f* localToEfb,
                            const GxGlyphDraw* glyph, float glyphW, float glyphH,
                            uint8_t r0, uint8_t g0, uint8_t b0,
                            uint8_t r1, uint8_t g1, uint8_t b1,
                            uint8_t r2, uint8_t g2, uint8_t b2,
                            uint8_t r3, uint8_t g3, uint8_t b3,
                            uint8_t a) {
    float lx0 = glyph->localX0;
    float ly0 = glyph->localY0;
    float lx1 = lx0 + glyphW;
    float ly1 = ly0 + glyphH;

    float sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3;
    TRANSFORM_CORNER(localToEfb, lx0, ly0, gx->portW, gx->portH, gx->portX, gx->portY, sx0, sy0);
    TRANSFORM_CORNER(localToEfb, lx1, ly0, gx->portW, gx->portH, gx->portX, gx->portY, sx1, sy1);
    TRANSFORM_CORNER(localToEfb, lx1, ly1, gx->portW, gx->portH, gx->portX, gx->portY, sx2, sy2);
    TRANSFORM_CORNER(localToEfb, lx0, ly1, gx->portW, gx->portH, gx->portX, gx->portY, sx3, sy3);

    // Glyph corner colors are usually uniform; use top-left for the shared emitter.
    (void)r1; (void)g1; (void)b1; (void)r2; (void)g2; (void)b2; (void)r3; (void)g3; (void)b3;
    emitTexturedAtlasRect(gx, glyph->pageId,
        glyph->sourceX, glyph->sourceY, glyph->sourceW, glyph->sourceH,
        sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3, r0, g0, b0, a);
}

static void gxDrawText(Renderer* renderer, const char* text,
                       float x, float y, float xscale, float yscale,
                       float angleDeg, float lineSeparation) {
    if (text == nullptr || text[0] == '\0') return;

    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    DataWin* dw = renderer->dataWin;
    int32_t fontIndex = renderer->drawFont;
    if (fontIndex < 0 || dw->font.count <= (uint32_t)fontIndex) return;

    Font* font = &dw->font.fonts[fontIndex];
    GxFontState fontState;
    if (!gxResolveFontState(gx, dw, font, &fontState)) return;

    uint32_t color = renderer->drawColor;
    uint8_t r = BGR_R(color), g = BGR_G(color), b = BGR_B(color);
    uint8_t a = alphaToU8(renderer->drawAlpha);

    int32_t textLen = (int32_t)strlen(text);
    int32_t lineCount = TextUtils_countLines(text, textLen);
    float lineStride = (lineSeparation < 0.0f)
        ? TextUtils_lineStride(font)
        : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));

    float totalHeight = (float)lineCount * lineStride;
    float valignOffset = 0.0f;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -angleDeg * ((float)M_PI / 180.0f);
    Matrix4f textT;
    Matrix4f_setTransform2D(&textT, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);
    Matrix4f localToEfb;
    Matrix4f_multiply(&localToEfb, &gx->wvp, &textT);

    float cursorY = valignOffset - (float)font->ascenderOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineIdx < lineCount; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (lineEnd < textLen && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0.0f;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = false;
        if (pos < lineLen) {
            ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            hasCh = true;
        }

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            uint16_t nextCh = 0;
            bool hasNext = pos < lineLen;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

            if (glyph != nullptr) {
                bool drew = false;
                if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    GxGlyphDraw gd;
                    if (gxResolveGlyph(gx, dw, &fontState, glyph, cursorX, cursorY, &gd)) {
                        gxEmitGlyphQuad(gx, &localToEfb, &gd,
                                        (float)glyph->sourceWidth, (float)glyph->sourceHeight,
                                        r, g, b, r, g, b, r, g, b, r, g, b, a);
                        drew = true;
                    }
                }
                cursorX += (float)glyph->shift;
                if (drew && hasNext) cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }

            ch = nextCh;
            hasCh = hasNext;
        }

        cursorY += lineStride;
        if (lineEnd < textLen) lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        else lineStart = lineEnd;
    }
}

static void gxDrawTextColor(Renderer* renderer, const char* text,
                            float x, float y, float xscale, float yscale,
                            float angleDeg,
                            int32_t _c1, int32_t _c2, int32_t _c3, int32_t _c4,
                            float alpha, float lineSeparation) {
    if (text == nullptr || text[0] == '\0') return;

    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    DataWin* dw = renderer->dataWin;
    int32_t fontIndex = renderer->drawFont;
    if (fontIndex < 0 || dw->font.count <= (uint32_t)fontIndex) return;

    Font* font = &dw->font.fonts[fontIndex];
    GxFontState fontState;
    if (!gxResolveFontState(gx, dw, font, &fontState)) return;

    uint8_t a = alphaToU8(alpha);
    int32_t textLen = (int32_t)strlen(text);
    int32_t lineCount = TextUtils_countLines(text, textLen);
    float lineStride = (lineSeparation < 0.0f)
        ? TextUtils_lineStride(font)
        : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));

    float totalHeight = (float)lineCount * lineStride;
    float valignOffset = 0.0f;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -angleDeg * ((float)M_PI / 180.0f);
    Matrix4f textT;
    Matrix4f_setTransform2D(&textT, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);
    Matrix4f localToEfb;
    Matrix4f_multiply(&localToEfb, &gx->wvp, &textT);

    float cursorY = valignOffset - (float)font->ascenderOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineIdx < lineCount; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (lineEnd < textLen && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0.0f;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        float gradientX = 0.0f;
        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = false;
        if (pos < lineLen) {
            ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            hasCh = true;
        }

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            uint16_t nextCh = 0;
            bool hasNext = pos < lineLen;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

            if (glyph != nullptr) {
                float advance = (float)glyph->shift;
                float leftFrac = (lineWidth > 0.0f) ? (gradientX / lineWidth) : 0.0f;
                float rightFrac = (lineWidth > 0.0f) ? ((gradientX + advance) / lineWidth) : 1.0f;
                int32_t c1 = Color_lerp(_c1, _c2, leftFrac);
                int32_t c2 = Color_lerp(_c1, _c2, rightFrac);
                int32_t c3 = Color_lerp(_c4, _c3, rightFrac);
                int32_t c4 = Color_lerp(_c4, _c3, leftFrac);

                bool drew = false;
                if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    GxGlyphDraw gd;
                    if (gxResolveGlyph(gx, dw, &fontState, glyph, cursorX, cursorY, &gd)) {
                        gxEmitGlyphQuad(gx, &localToEfb, &gd,
                                        (float)glyph->sourceWidth, (float)glyph->sourceHeight,
                                        BGR_R(c1), BGR_G(c1), BGR_B(c1),
                                        BGR_R(c2), BGR_G(c2), BGR_B(c2),
                                        BGR_R(c3), BGR_G(c3), BGR_B(c3),
                                        BGR_R(c4), BGR_G(c4), BGR_B(c4),
                                        a);
                        drew = true;
                    }
                }

                cursorX += advance;
                gradientX += advance;
                if (drew && hasNext) {
                    float kern = TextUtils_getKerningOffset(glyph, nextCh);
                    cursorX += kern;
                    gradientX += kern;
                }
            }

            ch = nextCh;
            hasCh = hasNext;
        }

        cursorY += lineStride;
        if (lineEnd < textLen) lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        else lineStart = lineEnd;
    }
}

static void gxFlush(MAYBE_UNUSED Renderer* renderer) {}


// ===[ Vtable: blend / GPU state ]===

static BlendFactors gxGetBlendFactors(Renderer* renderer) {
    return ((GxRendererImpl*)renderer)->blendFactors;
}
static int32_t gxGetBlendMode(Renderer* renderer) {
    return ((GxRendererImpl*)renderer)->blendMode;
}
static void gxSetBlendMode(Renderer* renderer, int32_t mode) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    gx->blendMode = mode;
    applyBlend(gx);
}
static void gxSetBlendModeExt(Renderer* renderer,
    int32_t sfactor, int32_t dfactor,
    int32_t sfactor_alpha, int32_t dfactor_alpha)
{
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    gx->blendMode = bm_complex;
    gx->blendFactors.src      = sfactor;
    gx->blendFactors.dst      = dfactor;
    gx->blendFactors.srcAlpha = sfactor_alpha;
    gx->blendFactors.dstAlpha = dfactor_alpha;
    renderer->blendFactors    = gx->blendFactors;
    applyBlend(gx);
}
static void gxSetBlendEnable(Renderer* renderer, bool enable) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    gx->blendEnable = enable;
    applyBlend(gx);
}
static bool gxGetBlendEnable(Renderer* renderer) {
    return ((GxRendererImpl*)renderer)->blendEnable;
}
static void gxSetAlphaTestEnable(Renderer* renderer, bool enable) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    gx->alphaTestEnable = enable;
    if (enable) {
        GX_SetAlphaCompare(GX_GREATER, (u8)gx->alphaTestRef, GX_AOP_AND, GX_ALWAYS, 0);
    } else {
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    }
}
static void gxSetAlphaTestRef(Renderer* renderer, uint8_t ref) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    gx->alphaTestRef = ref;
    if (gx->alphaTestEnable) {
        GX_SetAlphaCompare(GX_GREATER, (u8)ref, GX_AOP_AND, GX_ALWAYS, 0);
    }
}
static void gxSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue, bool alpha) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    gx->colorWrite[0] = red;
    gx->colorWrite[1] = green;
    gx->colorWrite[2] = blue;
    gx->colorWrite[3] = alpha;
    bool anyRGB = red || green || blue;
    GX_SetColorUpdate(anyRGB ? GX_TRUE : GX_FALSE);
    GX_SetAlphaUpdate(alpha ? GX_TRUE : GX_FALSE);
}
static void gxGetColorWriteEnable(Renderer* renderer, bool* red, bool* green, bool* blue, bool* alpha) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    *red   = gx->colorWrite[0];
    *green = gx->colorWrite[1];
    *blue  = gx->colorWrite[2];
    *alpha = gx->colorWrite[3];
}
static void gxSetFog(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED bool enable, MAYBE_UNUSED uint32_t color) {}

// ===[ Vtable: setMatrix ]===

static void gxSetMatrix(Renderer* renderer, int32_t matrixType, Matrix4f matrix) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    renderer->gmlMatrices[matrixType] = matrix;
    if (matrixType == MATRIX_WORLD || matrixType == MATRIX_VIEW || matrixType == MATRIX_PROJECTION) {
        Matrix4f world = renderer->gmlMatrices[MATRIX_WORLD];
        Matrix4f view  = renderer->gmlMatrices[MATRIX_VIEW];
        Matrix4f proj  = renderer->gmlMatrices[MATRIX_PROJECTION];
        Matrix4f wv, wvp;
        Matrix4f_multiply(&wv,  &view, &world);
        Matrix4f_multiply(&wvp, &proj, &wv);
        renderer->gmlMatrices[MATRIX_WORLD_VIEW]           = wv;
        renderer->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = wvp;
        gx->wvp = wvp;
    } else if (matrixType == MATRIX_WORLD_VIEW_PROJECTION) {
        gx->wvp = matrix;
    }
}

// ===[ Vtable: surfaces (stubs) ]===

static int32_t gxEnsureApplicationSurface(Renderer* renderer, int32_t width, int32_t height) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    gx->appSurfW = width;
    gx->appSurfH = height;
    return APPLICATION_SURFACE_ID;
}

static int32_t gxCreateSurface(MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED int32_t width, MAYBE_UNUSED int32_t height) {
    return -1;
}

static bool gxSurfaceExists(MAYBE_UNUSED Renderer* renderer, int32_t surfaceID) {
    return surfaceID == APPLICATION_SURFACE_ID;
}

static bool gxSetRenderTarget(MAYBE_UNUSED Renderer* renderer, int32_t surfaceID, bool implicitApplicationSurface) {
    (void)implicitApplicationSurface;
    return surfaceID == APPLICATION_SURFACE_ID;
}

static float gxGetSurfaceWidth(Renderer* renderer, int32_t surfaceID) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    if (surfaceID == APPLICATION_SURFACE_ID) return (float)gx->appSurfW;
    return 0.0f;
}
static float gxGetSurfaceHeight(Renderer* renderer, int32_t surfaceID) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    if (surfaceID == APPLICATION_SURFACE_ID) return (float)gx->appSurfH;
    return 0.0f;
}
static void gxDrawSurface(MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED int32_t surfaceID,
    MAYBE_UNUSED int32_t srcLeft, MAYBE_UNUSED int32_t srcTop,
    MAYBE_UNUSED int32_t srcWidth, MAYBE_UNUSED int32_t srcHeight,
    MAYBE_UNUSED float x, MAYBE_UNUSED float y,
    MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale,
    MAYBE_UNUSED float angleDeg, MAYBE_UNUSED uint32_t color,
    MAYBE_UNUSED float alpha) {}

static void gxDrawSurfaceTiled(MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED int32_t surfaceID,
    MAYBE_UNUSED float x, MAYBE_UNUSED float y,
    MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale,
    MAYBE_UNUSED float roomW, MAYBE_UNUSED float roomH,
    MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {}

static void gxSurfaceResize(MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED int32_t surfaceID,
    MAYBE_UNUSED int32_t width, MAYBE_UNUSED int32_t height) {}

static void gxSurfaceFree(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID) {}

static void gxSurfaceCopy(MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED int32_t destSurfaceID,
    MAYBE_UNUSED int32_t destX, MAYBE_UNUSED int32_t destY,
    MAYBE_UNUSED int32_t srcSurfaceID,
    MAYBE_UNUSED int32_t srcX, MAYBE_UNUSED int32_t srcY,
    MAYBE_UNUSED int32_t srcW, MAYBE_UNUSED int32_t srcH,
    MAYBE_UNUSED bool part) {}

static bool gxSurfaceGetPixels(MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED int32_t surfaceID, MAYBE_UNUSED uint8_t* outRGBA) {
    return false;
}

// ===[ Vtable: sprite/texture introspection ]===

static int32_t gxCreateSpriteFromSurface(MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED int32_t surfaceID,
    MAYBE_UNUSED int32_t x, MAYBE_UNUSED int32_t y,
    MAYBE_UNUSED int32_t w, MAYBE_UNUSED int32_t h,
    MAYBE_UNUSED bool removeback, MAYBE_UNUSED bool smooth,
    MAYBE_UNUSED int32_t xorig, MAYBE_UNUSED int32_t yorig) {
    return -1;
}

static void gxDeleteSprite(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t spriteIndex) {}

// Encode a TPAG index as a texture handle: handle = tpagIndex + 1 (0 means no texture).
static uint32_t gxSpriteGetTexture(Renderer* renderer, int32_t tpagIndex) {
    DataWin* dw = renderer->dataWin;
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return 0;
    return (uint32_t)(tpagIndex + 1);
}

static uint32_t gxSurfaceGetTexture(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID) {
    return 0;
}

static float gxTextureGetTexelWidth(Renderer* renderer, uint32_t texID) {
    if (texID == 0) return 0.0f;
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    int32_t tpagIndex = (int32_t)(texID - 1);
    DataWin* dw = renderer->dataWin;
    if ((uint32_t)tpagIndex >= dw->tpag.count) return 0.0f;
    int16_t pageId = dw->tpag.items[tpagIndex].texturePageId;
    if (pageId < 0 || (uint32_t)pageId >= gx->textureCount) return 0.0f;
    if (!gx->texW[pageId]) return 0.0f;
    return 1.0f / (float)gx->texW[pageId];
}

static float gxTextureGetTexelHeight(Renderer* renderer, uint32_t texID) {
    if (texID == 0) return 0.0f;
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    int32_t tpagIndex = (int32_t)(texID - 1);
    DataWin* dw = renderer->dataWin;
    if ((uint32_t)tpagIndex >= dw->tpag.count) return 0.0f;
    int16_t pageId = dw->tpag.items[tpagIndex].texturePageId;
    if (pageId < 0 || (uint32_t)pageId >= gx->textureCount) return 0.0f;
    if (!gx->texH[pageId]) return 0.0f;
    return 1.0f / (float)gx->texH[pageId];
}

static bool gxTextureGetUVs(Renderer* renderer, uint32_t texID, float* outUVs) {
    if (texID == 0) return false;
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    int32_t tpagIndex = (int32_t)(texID - 1);
    DataWin* dw = renderer->dataWin;
    if ((uint32_t)tpagIndex >= dw->tpag.count) return false;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (pageId < 0 || (uint32_t)pageId >= gx->textureCount) return false;
    if (!gx->texW[pageId] || !gx->texH[pageId]) return false;
    int32_t mx, my, mw, mh;
    mapAtlasRect(gx, pageId, tpag->sourceX, tpag->sourceY, tpag->sourceWidth, tpag->sourceHeight,
                 &mx, &my, &mw, &mh);
    float tw = (float)gx->texW[pageId];
    float th = (float)gx->texH[pageId];
    outUVs[0] = (float)mx / tw;
    outUVs[1] = (float)my / th;
    outUVs[2] = (float)(mx + mw) / tw;
    outUVs[3] = (float)(my + mh) / th;
    return true;
}

static void gxTextureSetStage(MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED int32_t slot, MAYBE_UNUSED uint32_t texID) {}

// ===[ Vtable: shaders (all stubs) ]===

static void gxGpuSetShader(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t shaderIndex) {}
static void gxGpuResetShader(MAYBE_UNUSED Renderer* renderer) {}
static int32_t gxShaderGetUniform(MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED int32_t shaderIndex, MAYBE_UNUSED char* uniform) { return -1; }
static int32_t gxShaderGetSamplerIndex(MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED int32_t shaderIndex, MAYBE_UNUSED char* uniform) { return -1; }
static void gxShaderSetUniformF(MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED int32_t handle, MAYBE_UNUSED int32_t count,
    MAYBE_UNUSED float v1, MAYBE_UNUSED float v2,
    MAYBE_UNUSED float v3, MAYBE_UNUSED float v4) {}
static void gxShaderSetUniformFArray(MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED int32_t handle, MAYBE_UNUSED float* values,
    MAYBE_UNUSED uint32_t count) {}
static void gxShaderSetUniformI(MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED int32_t handle, MAYBE_UNUSED int32_t count,
    MAYBE_UNUSED int32_t v1, MAYBE_UNUSED int32_t v2,
    MAYBE_UNUSED int32_t v3, MAYBE_UNUSED int32_t v4) {}
static bool gxShaderIsCompiled(MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED int32_t shader) { return false; }
static bool gxShadersSupported(void) { return false; }

// ===[ Vtable table ]===

static RendererVtable s_gxVtable = {
    .init                    = gxInit,
    .destroy                 = gxDestroy,
    .beginFrame              = gxBeginFrame,
    .endFrameInit            = gxEndFrameInit,
    .endFrameEnd             = gxEndFrameEnd,
    .beginView               = gxBeginView,
    .endView                 = gxEndView,
    .applyProjection         = gxApplyProjection,
    .beginGUI                = gxBeginGUI,
    .setGuiProjection        = gxSetGuiProjection,
    .endGUI                  = gxEndGUI,
    .drawSprite              = gxDrawSprite,
    .drawSpritePart          = gxDrawSpritePart,
    .drawSpritePos           = gxDrawSpritePos,
    .drawRectangle           = gxDrawRectangle,
    .drawRectangleColor      = gxDrawRectangleColor,
    .drawLine                = gxDrawLine,
    .drawTriangle            = gxDrawTriangle,
    .drawLineColor           = gxDrawLineColor,
    .drawText                = gxDrawText,
    .drawTextColor           = gxDrawTextColor,
    .flush                   = gxFlush,
    .clearScreen             = gxClearScreen,
    .createSpriteFromSurface = gxCreateSpriteFromSurface,
    .deleteSprite            = gxDeleteSprite,
    .gpuGetBlendFactors      = gxGetBlendFactors,
    .gpuGetBlendMode         = gxGetBlendMode,
    .gpuSetBlendMode         = gxSetBlendMode,
    .gpuSetBlendModeExt      = gxSetBlendModeExt,
    .gpuSetBlendEnable       = gxSetBlendEnable,
    .gpuSetAlphaTestEnable   = gxSetAlphaTestEnable,
    .gpuSetAlphaTestRef      = gxSetAlphaTestRef,
    .gpuSetColorWriteEnable  = gxSetColorWriteEnable,
    .gpuGetColorWriteEnable  = gxGetColorWriteEnable,
    .gpuGetBlendEnable       = gxGetBlendEnable,
    .gpuSetFog               = gxSetFog,
    .drawTile                = nullptr,
    .drawSpriteTiled         = gxDrawSpriteTiled,
    .createSurface           = gxCreateSurface,
    .surfaceExists           = gxSurfaceExists,
    .setRenderTarget         = gxSetRenderTarget,
    .ensureApplicationSurface = gxEnsureApplicationSurface,
    .getSurfaceWidth         = gxGetSurfaceWidth,
    .getSurfaceHeight        = gxGetSurfaceHeight,
    .drawSurface             = gxDrawSurface,
    .drawSurfaceTiled        = gxDrawSurfaceTiled,
    .surfaceResize           = gxSurfaceResize,
    .surfaceFree             = gxSurfaceFree,
    .surfaceCopy             = gxSurfaceCopy,
    .surfaceGetPixels        = gxSurfaceGetPixels,
    .drawTiledPart           = nullptr,
    .gpuSetShader            = gxGpuSetShader,
    .gpuResetShader          = gxGpuResetShader,
    .shaderGetUniform        = gxShaderGetUniform,
    .shaderGetSamplerIndex   = gxShaderGetSamplerIndex,
    .shaderSetUniformF       = gxShaderSetUniformF,
    .shaderSetUniformFArray  = gxShaderSetUniformFArray,
    .shaderSetUniformI       = gxShaderSetUniformI,
    .spriteGetTexture        = gxSpriteGetTexture,
    .surfaceGetTexture       = gxSurfaceGetTexture,
    .textureGetTexelWidth    = gxTextureGetTexelWidth,
    .textureGetTexelHeight   = gxTextureGetTexelHeight,
    .textureGetUVs           = gxTextureGetUVs,
    .textureSetStage         = gxTextureSetStage,
    .shaderIsCompiled        = gxShaderIsCompiled,
    .shadersSupported        = gxShadersSupported,
    .setMatrix               = gxSetMatrix,
};

// ===[ Public API ]===

Renderer* GxRenderer_create(GXRModeObj* rmode, void* xfb0, void* xfb1, u32* fbIndex) {
    GxRendererImpl* gx = (GxRendererImpl*)safeCalloc(1, sizeof(GxRendererImpl));
    gx->base.vtable  = &s_gxVtable;
    gx->rmode        = rmode;
    gx->xfb[0]       = xfb0;
    gx->xfb[1]       = xfb1;
    gx->fbIndex      = fbIndex;
    gx->base.drawColor = 0xFFFFFF;
    gx->base.drawAlpha = 1.0f;
    gx->base.drawFont  = -1;
    gx->base.currentShader = -1;
    gx->base.circlePrecision = 24;
    return (Renderer*)gx;
}

void GxRenderer_present(Renderer* renderer, bool waitVsync) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;

    GX_DrawDone();

    // clearEfb=false: keep EFB pixels so presentDuplicate can CopyDisp again
    // on the odd VI without a CPU XFB memcpy (that caused black frames + audio hitches).
    GXColor clearClr = {0, 0, 0, 255};
    GX_SetCopyClear(clearClr, GX_MAX_Z24);
    GX_CopyDisp(gx->xfb[*gx->fbIndex], GX_FALSE);

    VIDEO_SetNextFramebuffer(gx->xfb[*gx->fbIndex]);
    VIDEO_Flush();
    if (waitVsync) VIDEO_WaitVSync();
    *gx->fbIndex ^= 1;
}

void GxRenderer_presentDuplicate(Renderer* renderer) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;

    // EFB still holds the just-presented frame (present used clearEfb=false).
    // No GX_DrawDone — nothing was submitted since present; keep this cheap for AESND.
    GX_CopyDisp(gx->xfb[*gx->fbIndex], GX_FALSE);
    VIDEO_SetNextFramebuffer(gx->xfb[*gx->fbIndex]);
    VIDEO_Flush();
    *gx->fbIndex ^= 1;
}

void GxRenderer_pumpTexLoads(Renderer* renderer, uint64_t budgetNs) {
    if (!renderer || budgetNs == 0) return;
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    gx->texColdLoadsFrame = 0;
    gx->texEvictsFrame = 0;
    gx->texDecodeNsFrame = 0;
    uint64_t t0 = nowNanos();

    // One wanted slice at a time. Prefer the page a draw touched most recently
    // so battle sprites cannot starve the font atlas by sequentially filling
    // unused tiles of a lower-numbered page.
    while ((nowNanos() - t0) < budgetNs) {
        uint32_t best = UINT32_MAX;
        uint64_t bestUsed = 0;
        bool bestPending = false;
        for (uint32_t i = 0; i < gx->textureCount; i++) {
            bool pending = pageHasPendingSlices(gx, i);
            bool wanted = gx->texWanted && gx->texWanted[i];
            if (!pending && !wanted) continue;
            if (gx->texLoaded[i] && gx->texW[i] == 0) {
                if (gx->texWanted) gx->texWanted[i] = false;
                continue;
            }
            uint64_t used = gx->texLastUsed ? gx->texLastUsed[i] : 0;
            if (best == UINT32_MAX ||
                (pending && !bestPending) ||
                (pending == bestPending && used > bestUsed) ||
                (pending == bestPending && used == bestUsed && i < best)) {
                best = i;
                bestUsed = used;
                bestPending = pending;
            }
        }
        if (best == UINT32_MAX) return;
        advanceTexLoad(gx, best);
        // WTL2 memcpy is cheap enough to fill several wanted slices in one spare VI.
        // PNG/WTL1 inflate overruns the budget, so the next while-check stops us.
    }
}

void GxRenderer_queryTextureStats(Renderer* renderer, GxTextureStats* out) {
    if (!out) return;
    out->totalPages = 0;
    out->loadedPages = 0;
    out->residentBytes = 0;
    out->coldLoadsThisFrame = 0;
    out->deferredLoadsThisFrame = 0;
    out->evictsThisFrame = 0;
    out->decodeMsThisFrame = 0.0f;
    if (!renderer) return;

    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    out->totalPages = gx->textureCount;
    out->coldLoadsThisFrame = gx->texColdLoadsFrame;
    out->deferredLoadsThisFrame = gx->texDeferredFrame;
    out->evictsThisFrame = gx->texEvictsFrame;
    out->decodeMsThisFrame = (float)((double)gx->texDecodeNsFrame / 1e6);
    for (uint32_t i = 0; i < gx->textureCount; i++) {
        if (!gx->texLoaded || !gx->texLoaded[i]) continue;
        if (!gx->texW || gx->texW[i] == 0) continue;
        out->loadedPages++;
        if (!gx->texSlices || !gx->texSliceCounts) continue;
        for (int32_t s = 0; s < gx->texSliceCounts[i]; s++) {
            GxTextureSlice* slice = &gx->texSlices[i][s];
            if (slice->data) {
                out->residentBytes += (uint64_t)gx->texW[i] * (uint64_t)slice->height * (uint64_t)GX_TEX_BPP;
            }
        }
    }
}
