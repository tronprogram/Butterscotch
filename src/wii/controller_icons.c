#include "controller_icons.h"

#include "utils.h"

#include <gccore.h>
#include <malloc.h>
#include <string.h>

typedef struct {
    bool ready;
    GXTexObj tex;
    uint8_t* texData;
} WiiCtrlIconsState;

static WiiCtrlIconsState gIcons;

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

void WiiCtrlIcons_init(void) {
    if (gIcons.ready) return;

    size_t linearBytes = (size_t)WII_CTRL_ATLAS_W * (size_t)WII_CTRL_ATLAS_H * 4;
    uint8_t* linear = (uint8_t*)safeMalloc(linearBytes);
    memcpy(linear, wiiCtrlIconPixels, linearBytes);

    gIcons.texData = (uint8_t*)memalign(32, linearBytes);
    requireNotNull(gIcons.texData);
    convertLinearRgba8ToTiled(gIcons.texData, linear, WII_CTRL_ATLAS_W, WII_CTRL_ATLAS_H);
    free(linear);
    DCFlushRange(gIcons.texData, linearBytes);

    GX_InitTexObj(&gIcons.tex, gIcons.texData, WII_CTRL_ATLAS_W, WII_CTRL_ATLAS_H,
                  GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&gIcons.tex, GX_NEAR, GX_NEAR);
    GX_InvalidateTexAll();
    gIcons.ready = true;
}

float WiiCtrlIcons_advance(WiiCtrlIconId id, float scale) {
    if ((int)id < 0 || (int)id >= WII_CTRL_ICON_COUNT) return 0.0f;
    return (float)wiiCtrlIconGlyphs[id].xadvance * scale;
}

void WiiCtrlIcons_draw(WiiCtrlIconId id, float x, float y, float scale,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!gIcons.ready) WiiCtrlIcons_init();
    if ((int)id < 0 || (int)id >= WII_CTRL_ICON_COUNT) return;

    const WiiCtrlIconGlyph* glyph = &wiiCtrlIconGlyphs[id];
    if (glyph->w == 0 || glyph->h == 0) return;

    float qx0 = x + (float)glyph->xoffset * scale;
    float qy0 = y + (float)glyph->yoffset * scale;
    float qx1 = qx0 + (float)glyph->w * scale;
    float qy1 = qy0 + (float)glyph->h * scale;
    float u0 = (float)glyph->x / (float)WII_CTRL_ATLAS_W;
    float v0 = (float)glyph->y / (float)WII_CTRL_ATLAS_H;
    float u1 = ((float)glyph->x + (float)glyph->w) / (float)WII_CTRL_ATLAS_W;
    float v1 = ((float)glyph->y + (float)glyph->h) / (float)WII_CTRL_ATLAS_H;

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
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GX_LoadTexObj(&gIcons.tex, GX_TEXMAP0);

    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
        GX_Position2f32(qx0, qy0); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u0, v0);
        GX_Position2f32(qx1, qy0); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u1, v0);
        GX_Position2f32(qx1, qy1); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u1, v1);
        GX_Position2f32(qx0, qy1); GX_Color4u8(r, g, b, a); GX_TexCoord2f32(u0, v1);
    GX_End();
}
