#include "mem2_scratch.h"

#include <ogc/system.h>
#include <stdint.h>
#include <string.h>

#include "log.h"

// 2048² RGBA = 16MB + PNG filter/zlib realloc churn on the bump allocator.
#define MEM2_SCRATCH_BYTES     (28u * 1024u * 1024u)
#define MEM2_SCRATCH_KEEP_HEAP (20u * 1024u * 1024u)

static uint8_t* s_base = NULL;
static uint8_t* s_cur  = NULL;
static uint8_t* s_end  = NULL;
static int s_active = 0;

void Mem2Scratch_reserve(void) {
    if (s_base) return;

    u8* lo = (u8*)SYS_GetArena2Lo();
    u8* hi = (u8*)SYS_GetArena2Hi();
    if (!hi || hi <= lo) {
        logWarn("Mem2Scratch: Arena2 unavailable\n");
        return;
    }

    u32 avail = (u32)(hi - lo);
    u32 want = MEM2_SCRATCH_BYTES;
    if (avail > MEM2_SCRATCH_KEEP_HEAP) {
        u32 maxWant = (avail - MEM2_SCRATCH_KEEP_HEAP) & ~31u;
        if (want > maxWant) want = maxWant;
    } else {
        want = (avail / 3) & ~31u; // desperate: keep most of MEM2 for the heap
    }
    if (want < 10u * 1024u * 1024u) {
        logWarn("Mem2Scratch: Arena2 too small to reserve decode scratch (avail=%u)\n", avail);
        return;
    }

    u8* newHi = (u8*)(((u32)hi - want) & ~31u);
    if (newHi <= lo) {
        logWarn("Mem2Scratch: carve failed\n");
        return;
    }

    SYS_SetArena2Hi(newHi);
    s_base = newHi;
    s_cur = s_base;
    s_end = s_base + want;
    logInfo("Mem2Scratch: reserved %.1f MB from Arena2 Hi (heap keeps ~%.1f MB MEM2)\n",
            (double)want / (1024.0 * 1024.0),
            (double)(avail - want) / (1024.0 * 1024.0));
}

void Mem2Scratch_begin(void) {
    if (!s_base) {
        s_active = 0;
        return;
    }
    s_cur = s_base;
    s_active = 1;
}

void Mem2Scratch_end(void) {
    s_active = 0;
    s_cur = s_base;
}

int Mem2Scratch_active(void) {
    return s_active;
}

void* Mem2Scratch_alloc(size_t bytes) {
    if (!s_active || !s_base) return NULL;
    size_t n = (bytes + 31u) & ~31u;
    if (s_cur + n > s_end) return NULL;
    void* p = s_cur;
    s_cur += n;
    return p;
}
