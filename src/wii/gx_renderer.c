#include "gx_renderer.h"
#include "matrix_math.h"
#include "utils.h"
#include "data_win.h"
#include "image_decoder.h"
#include "runner.h"
#include "stdio_compat.h"
#include "math_compat.h"
#include "string_compat.h"

#include <gccore.h>
#include <ogc/gx.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

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
    bool* texLoaded;
    GXTexObj whiteTex;
    uint8_t* whiteTexData;
    int32_t appSurfW, appSurfH;
} GxRendererImpl;

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

static bool ensureTexLoaded(GxRendererImpl* gx, uint32_t pageId) {
    if (gx->texLoaded[pageId]) return gx->texW[pageId] != 0;
    gx->texLoaded[pageId] = true;

    DataWin* dw = gx->base.dataWin;
    Texture* txtr = &dw->txtr.textures[pageId];
    DataWin_loadTxtrIfNeeded(dw, pageId);
    if (!txtr->blobData) return false;

    int w = 0, h = 0;
    bool gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);
    uint8_t* pixels = ImageDecoder_decodeToRgba(txtr->blobData, (size_t)txtr->blobSize, gm2022_5, &w, &h);
    if (!pixels) {
        logWarn("GxRenderer: Failed to decode TXTR page %u\n", pageId);
        return false;
    }

    if (!txtr->mapped) {
        free(txtr->blobData);
        txtr->blobData = nullptr;
    }

    // Wii RGBA8 requires 4x4 tile alignment, and GX texture dimensions are capped at 1024.
    // GameMaker atlases commonly use 1024x2048 pages, so keep their coordinate space but
    // upload them as <=1024-high vertical slices selected per TPAG at draw time.
    int pw = (w + 3) & ~3;
    int ph = (h + 3) & ~3;
    if (pw > 1024) {
        free(pixels);
        logWarn("GxRenderer: TXTR page %u is %d pixels wide; GX maximum is 1024\n", pageId, pw);
        return false;
    }

    int sliceCount = (ph + 1023) / 1024;
    GxTextureSlice* slices = (GxTextureSlice*)safeCalloc((size_t)sliceCount, sizeof(GxTextureSlice));
    int tilesX = pw / 4;
    for (int sliceIndex = 0; sliceIndex < sliceCount; sliceIndex++) {
        GxTextureSlice* slice = &slices[sliceIndex];
        slice->y = sliceIndex * 1024;
        slice->height = ph - slice->y;
        if (slice->height > 1024) slice->height = 1024;
        size_t bufSize = (size_t)pw * (size_t)slice->height * 4;
        slice->data = (uint8_t*)memalign(32, bufSize);
        if (!slice->data) {
            for (int i = 0; i < sliceIndex; i++) free(slices[i].data);
            free(slices);
            free(pixels);
            logWarn("GxRenderer: memalign failed for TXTR page %u slice %d\n", pageId, sliceIndex);
            return false;
        }
        memset(slice->data, 0, bufSize);

        // Convert RGBA linear to Wii RGBA8 tiled (4x4 tiles, AR plane then GB plane).
        int tilesY = slice->height / 4;
        for (int ty = 0; ty < tilesY; ty++) {
            for (int tx = 0; tx < tilesX; tx++) {
                uint8_t* tile = slice->data + (ty * tilesX + tx) * 64;
                for (int row = 0; row < 4; row++) {
                    for (int col = 0; col < 4; col++) {
                        int px = tx * 4 + col;
                        int py = slice->y + ty * 4 + row;
                        uint8_t R = 0, G = 0, B = 0, A = 0;
                        if (px < w && py < h) {
                            const uint8_t* p = pixels + (py * w + px) * 4;
                            R = p[0]; G = p[1]; B = p[2]; A = p[3];
                        }
                        int idx = row * 4 + col;
                        tile[idx * 2]          = A;
                        tile[idx * 2 + 1]      = R;
                        tile[32 + idx * 2]     = G;
                        tile[32 + idx * 2 + 1] = B;
                    }
                }
            }
        }

        GX_InitTexObj(&slice->obj, slice->data, (u16)pw, (u16)slice->height,
                      GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjFilterMode(&slice->obj, GX_NEAR, GX_NEAR);
        DCFlushRange(slice->data, bufSize);
    }

    free(pixels);
    GX_InvalidateTexAll();

    gx->texSlices[pageId] = slices;
    gx->texSliceCounts[pageId] = sliceCount;
    gx->texW[pageId] = pw;
    gx->texH[pageId] = ph;

    logInfo("GxRenderer: Loaded TXTR page %u (%dx%d padded to %dx%d, %d GX slice%s)\n",
        pageId, w, h, pw, ph, sliceCount, sliceCount == 1 ? "" : "s");
    return true;
}

static bool resolveTpag(GxRendererImpl* gx, int32_t tpagIndex,
                        TexturePageItem** outTpag, int32_t* outPageId) {
    DataWin* dw = gx->base.dataWin;
    if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) return false;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (pageId < 0 || (uint32_t)pageId >= gx->textureCount) return false;
    if (!ensureTexLoaded(gx, (uint32_t)pageId)) return false;
    *outTpag = tpag;
    *outPageId = (int32_t)pageId;
    return true;
}

static GxTextureSlice* resolveTextureSlice(GxRendererImpl* gx, int32_t pageId,
                                           int32_t sourceY, int32_t sourceHeight) {
    if (sourceY < 0 || sourceHeight <= 0) return nullptr;
    int32_t sliceIndex = sourceY / 1024;
    if (sliceIndex < 0 || sliceIndex >= gx->texSliceCounts[pageId]) return nullptr;
    GxTextureSlice* slice = &gx->texSlices[pageId][sliceIndex];
    if (sourceY + sourceHeight > slice->y + slice->height) {
        logWarn("GxRenderer: texture rect y=%d..%d crosses GX slice boundary on page %d\n",
            sourceY, sourceY + sourceHeight, pageId);
        return nullptr;
    }
    return slice;
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
    gx->texLoaded = (bool*)safeCalloc(gx->textureCount, sizeof(bool));

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
        for (int32_t slice = 0; slice < gx->texSliceCounts[i]; slice++) {
            free(gx->texSlices[i][slice].data);
        }
        free(gx->texSlices[i]);
    }
    free(gx->texSlices);
    free(gx->texSliceCounts);
    free(gx->texW);
    free(gx->texH);
    free(gx->texLoaded);
    free(gx->whiteTexData);
    free(gx);
}

// ===[ Vtable: frame ]===

static void gxBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
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

    // Use the runner's expanded view rect (same as mouse mapping), not Orthographic(-H)+LookAt.
    Matrix4f viewMatrix;
    Matrix4f_identity(&viewMatrix);
    Matrix4f projMatrix;
    Matrix4f_viewProjection(&projMatrix,
        (float)viewX, (float)viewY, (float)viewW, (float)viewH, viewAngle);
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
    GxTextureSlice* slice = resolveTextureSlice(gx, pageId, tpag->sourceY, tpag->sourceHeight);
    if (!slice) return;

    float tw = (float)gx->texW[pageId];
    float th = (float)slice->height;
    float u0 = (float)tpag->sourceX / tw;
    float v0 = (float)(tpag->sourceY - slice->y) / th;
    float u1 = (float)(tpag->sourceX + tpag->sourceWidth)  / tw;
    float v1 = (float)(tpag->sourceY - slice->y + tpag->sourceHeight) / th;

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

    GX_LoadTexObj(&slice->obj, GX_TEXMAP0);
    setTEVTextured();
    applyBlend(gx);
    emitTexQuad(sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3, u0, v0, u1, v1, r, g, b, a);
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
    int32_t sourceY = (int32_t)tpag->sourceY + srcOffY;
    GxTextureSlice* slice = resolveTextureSlice(gx, pageId, sourceY, srcH);
    if (!slice) return;

    float tw = (float)gx->texW[pageId];
    float th = (float)slice->height;
    float u0 = (float)(tpag->sourceX + srcOffX) / tw;
    float v0 = (float)(sourceY - slice->y) / th;
    float u1 = (float)(tpag->sourceX + srcOffX + srcW) / tw;
    float v1 = (float)(sourceY - slice->y + srcH) / th;

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

    GX_LoadTexObj(&slice->obj, GX_TEXMAP0);
    setTEVTextured();
    applyBlend(gx);
    emitTexQuad(sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3, u0, v0, u1, v1, r, g, b, a);
}

static void gxDrawSpritePos(Renderer* renderer, int32_t tpagIndex,
    float x1, float y1, float x2, float y2,
    float x3, float y3, float x4, float y4, float alpha)
{
    GxRendererImpl* gx = (GxRendererImpl*)renderer;
    TexturePageItem* tpag;
    int32_t pageId;
    if (!resolveTpag(gx, tpagIndex, &tpag, &pageId)) return;
    GxTextureSlice* slice = resolveTextureSlice(gx, pageId, tpag->sourceY, tpag->sourceHeight);
    if (!slice) return;

    float tw = (float)gx->texW[pageId];
    float th = (float)slice->height;
    float u0 = (float)tpag->sourceX / tw;
    float v0 = (float)(tpag->sourceY - slice->y) / th;
    float u1 = (float)(tpag->sourceX + tpag->sourceWidth)  / tw;
    float v1 = (float)(tpag->sourceY - slice->y + tpag->sourceHeight) / th;

    float sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3;
    TRANSFORM_CORNER(&gx->wvp, x1, y1, gx->portW, gx->portH, gx->portX, gx->portY, sx0, sy0);
    TRANSFORM_CORNER(&gx->wvp, x2, y2, gx->portW, gx->portH, gx->portX, gx->portY, sx1, sy1);
    TRANSFORM_CORNER(&gx->wvp, x3, y3, gx->portW, gx->portH, gx->portX, gx->portY, sx2, sy2);
    TRANSFORM_CORNER(&gx->wvp, x4, y4, gx->portW, gx->portH, gx->portX, gx->portY, sx3, sy3);

    uint8_t a = alphaToU8(alpha);
    GX_LoadTexObj(&slice->obj, GX_TEXMAP0);
    setTEVTextured();
    applyBlend(gx);
    emitTexQuad(sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3, u0, v0, u1, v1, 255, 255, 255, a);
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
    GxTextureSlice* slice = resolveTextureSlice(gx, pageId, tpag->sourceY, tpag->sourceHeight);
    if (!slice) return;

    float axs = fabsf(xscale);
    float ays = fabsf(yscale);
    float tileW = (float)tpag->boundingWidth  * axs;
    float tileH = (float)tpag->boundingHeight * ays;
    if (tileW < 0.5f || tileH < 0.5f) return;

    float tw = (float)gx->texW[pageId];
    float th = (float)slice->height;
    float u0 = (float)tpag->sourceX / tw;
    float v0 = (float)(tpag->sourceY - slice->y) / th;
    float u1 = (float)(tpag->sourceX + tpag->sourceWidth)  / tw;
    float v1 = (float)(tpag->sourceY - slice->y + tpag->sourceHeight) / th;

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

    GX_LoadTexObj(&slice->obj, GX_TEXMAP0);
    setTEVTextured();
    applyBlend(gx);

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
            emitTexQuad(sx0, sy0, sx1, sy1, sx2, sy2, sx3, sy3, u0, v0, u1, v1, r, g, b, a);
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

static void gxDrawText(MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED const char* text,
    MAYBE_UNUSED float x, MAYBE_UNUSED float y,
    MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale,
    MAYBE_UNUSED float angleDeg, MAYBE_UNUSED float lineSeparation) {}

static void gxDrawTextColor(MAYBE_UNUSED Renderer* renderer,
    MAYBE_UNUSED const char* text,
    MAYBE_UNUSED float x, MAYBE_UNUSED float y,
    MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale,
    MAYBE_UNUSED float angleDeg,
    MAYBE_UNUSED int32_t c1, MAYBE_UNUSED int32_t c2,
    MAYBE_UNUSED int32_t c3, MAYBE_UNUSED int32_t c4,
    MAYBE_UNUSED float alpha, MAYBE_UNUSED float lineSeparation) {}

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
    float tw = (float)gx->texW[pageId];
    float th = (float)gx->texH[pageId];
    outUVs[0] = (float)tpag->sourceX / tw;
    outUVs[1] = (float)tpag->sourceY / th;
    outUVs[2] = (float)(tpag->sourceX + tpag->sourceWidth)  / tw;
    outUVs[3] = (float)(tpag->sourceY + tpag->sourceHeight) / th;
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

void GxRenderer_present(Renderer* renderer) {
    GxRendererImpl* gx = (GxRendererImpl*)renderer;

    GX_DrawDone();

    GXColor clearClr = {0, 0, 0, 255};
    GX_SetCopyClear(clearClr, GX_MAX_Z24);
    GX_CopyDisp(gx->xfb[*gx->fbIndex], GX_TRUE);

    VIDEO_SetNextFramebuffer(gx->xfb[*gx->fbIndex]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    *gx->fbIndex ^= 1;
}
