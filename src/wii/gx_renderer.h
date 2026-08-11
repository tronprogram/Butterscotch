#ifndef _BS_GX_RENDERER_H_
#define _BS_GX_RENDERER_H_

#include "renderer.h"
#include <gccore.h>

typedef struct GxRenderer GxRenderer;

// GxRenderer_create: main initialises VIDEO + GX FIFO before calling this.
// rmode: video mode selected by main. xfb0/xfb1: two XFB buffers allocated by main.
// fbIndex: pointer to the active-buffer index owned by main (0 or 1); present flips it.
Renderer* GxRenderer_create(GXRModeObj* rmode, void* xfb0, void* xfb1, u32* fbIndex);

// GxRenderer_present: call from the main loop after the frame is drawn.
// Executes GX_DrawDone, EFB→XFB copy, VIDEO flip, WaitVSync, buffer swap.
void GxRenderer_present(Renderer* renderer);

#endif /* _BS_GX_RENDERER_H_ */
