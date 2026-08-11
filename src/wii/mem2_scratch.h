#ifndef _BS_MEM2_SCRATCH_H_
#define _BS_MEM2_SCRATCH_H_

#include <stddef.h>

// Carve a private MEM2 decode scratch from Arena2 *Hi* so it never overlaps
// malloc/sbrk (which grows Arena2Lo). Safe for large PNG decode peaks.

void Mem2Scratch_reserve(void); // call once early, before heavy malloc
void Mem2Scratch_begin(void);   // reset bump; subsequent allocs use scratch
void Mem2Scratch_end(void);     // stop redirecting allocs
int  Mem2Scratch_active(void);
void* Mem2Scratch_alloc(size_t bytes);

#endif
