#ifndef _BS_GX_RENDERER_H_
#define _BS_GX_RENDERER_H_

#include "renderer.h"
#include <gccore.h>

typedef struct GxRenderer GxRenderer;

typedef struct {
    uint32_t totalPages;
    uint32_t loadedPages;
    uint64_t residentBytes; // RGB5A3 slice storage currently held
    // Per-frame hitch diagnostics (reset in beginFrame).
    uint32_t coldLoadsThisFrame;
    uint32_t deferredLoadsThisFrame;
    uint32_t evictsThisFrame;
    float decodeMsThisFrame;
} GxTextureStats;

// GxRenderer_create: main initialises VIDEO + GX FIFO before calling this.
// rmode: video mode selected by main. xfb0/xfb1: two XFB buffers allocated by main.
// fbIndex: pointer to the active-buffer index owned by main (0 or 1); present flips it.
Renderer* GxRenderer_create(GXRModeObj* rmode, void* xfb0, void* xfb1, u32* fbIndex);

// GxRenderer_present: GX_DrawDone, EFB→XFB copy, VIDEO flip.
// If waitVsync is true, blocks on VIDEO_WaitVSync after the flip.
void GxRenderer_present(Renderer* renderer, bool waitVsync);

// Duplicate the last frame by EFB→XFB copying again without clearing.
// Requires the prior present() used clearEfb=false so the EFB still holds pixels.
// Used on non-step VIs: GML Draw stays at room_speed, present stays 60Hz.
void GxRenderer_presentDuplicate(Renderer* renderer);

// Decode/upload pending TXTR pages using up to budgetNs of wall time.
// Call outside the vsync-critical path so Step/Draw stay hitch-free.
void GxRenderer_pumpTexLoads(Renderer* renderer, uint64_t budgetNs);

void GxRenderer_queryTextureStats(Renderer* renderer, GxTextureStats* out);

#endif /* _BS_GX_RENDERER_H_ */
