#ifndef _BS_WII_OVERLAY_H_
#define _BS_WII_OVERLAY_H_

#include "runner.h"
#include <stdint.h>

typedef enum {
    WII_STATS_ENABLED = 0,
    WII_STATS_ENABLED_WITH_PROFILER = 1,
    WII_STATS_DISABLED = 2,
    WII_STATS_MAX
} WiiDebugOverlayState;

void WiiOverlay_init(void);
void WiiOverlay_deinit(void);

// Call once after XFBs are allocated (and before the heavy data.win load) so
// MEM1/MEM2 "used/total" reflect the post-video heap budget, not mallinfo lies.
void WiiOverlay_snapshotArenas(void);

WiiDebugOverlayState WiiOverlay_getDebugOverlayState(void);
void WiiOverlay_setDebugOverlayState(WiiDebugOverlayState state, Runner* runner);
void WiiOverlay_toggleDebugOverlay(Runner* runner);

// frameDeltaMs: wall-clock ms since previous display frame start (includes vsync).
// presentMs: GX_DrawDone + flip time for the previous present call.
// maxStepIntervalMs: recent max wall-clock gap between Runner_step calls.
void WiiOverlay_drawDebugOverlay(Renderer* renderer, const Runner* runner,
                                 float tickMs, float stepMs, float drawMs, float audioMs,
                                 float presentMs, float frameDeltaMs, float maxStepIntervalMs,
                                 int fbWidth, int fbHeight);

#endif /* _BS_WII_OVERLAY_H_ */
