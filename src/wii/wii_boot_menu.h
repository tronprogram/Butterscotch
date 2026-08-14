#ifndef _BS_WII_BOOT_MENU_H_
#define _BS_WII_BOOT_MENU_H_

#include "wii_settings.h"
#include "wii_games.h"

#include <gccore.h>
#include <stdint.h>

struct Runner;

typedef enum {
    WII_BOOT_START_GAME = 0,
    WII_BOOT_RETURN_TO_MENU = 1,
} WiiBootResult;

// Interactive shell before data.win / runner. Blocks until Start or Exit.
// Uses DebugFont + GX; requires VIDEO/GX/FAT already up. Initializes WPAD.
WiiBootResult WiiBootMenu_run(
    GXRModeObj* rmode,
    void* xfb0,
    void* xfb1,
    u32* fbIndex,
    const char* bundleDir,
    WiiPortSettings* settings,
    const WiiGameEntry* games,
    int gameCount,
    int* selectedGame
);

// In-game HOME system menu. Returns true if caller should soft-reset via game_restart.
// Returns false for resume. Sets *exitToLoader if user chose Exit.
bool WiiSystemMenu_run(
    GXRModeObj* rmode,
    void* xfb0,
    void* xfb1,
    u32* fbIndex,
    const char* bundleDir,
    WiiPortSettings* settings,
    struct Runner* runner,
    bool* exitToLoader
);

#endif /* _BS_WII_BOOT_MENU_H_ */
