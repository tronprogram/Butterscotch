#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/stat.h>

#include <gccore.h>
#include <ogc/system.h>
#include <wiiuse/wpad.h>
#include <fat.h>

#include "runner.h"
#include "runner_keyboard.h"
#include "vm.h"
#include "../data_win.h"
#include "../json_reader.h"
#include "../overlay_file_system.h"
#include "gx_renderer.h"
#include "noop_audio_system.h"
#include "wii_overlay.h"
#include "wii_boot_menu.h"
#include "wii_settings.h"
#include "../utils.h"
#include "../gettime.h"

#ifdef USE_WII_AUDIO
#include "wii_audio_system.h"
#endif

#define GX_FIFO_SIZE (256 * 1024)

// Button to GML key mapping (live table; seeded from WiiPortSettings / CONFIG.JSN)
typedef struct {
    uint32_t wpadButton;
    int32_t  gmlKey;
} WiiMapping;

static WiiMapping* wiiMappings  = NULL;
static int         wiiMappingCount = 0;
static WiiMapping  wiiMappingsOwned[WII_SETTINGS_MAP_MAX];

static uint32_t prevHeld = 0;

static GXRModeObj* gRmode = NULL;
static void* gXfb0 = NULL;
static void* gXfb1 = NULL;
static u32* gFbIndex = NULL;
static const char* gBundleDir = NULL;
static WiiPortSettings gPortSettings;
static AudioSystem* gAudioSystem = NULL;

static void installDefaultMappings(void);

static void applyPortMappings(const WiiPortSettings* s) {
    int n = s->mapCount;
    if (n > WII_SETTINGS_MAP_MAX) n = WII_SETTINGS_MAP_MAX;
    if (n <= 0) {
        installDefaultMappings();
        return;
    }
    for (int i = 0; i < n; i++) {
        wiiMappingsOwned[i].wpadButton = s->maps[i].wpadButton;
        wiiMappingsOwned[i].gmlKey = s->maps[i].gmlKey;
    }
    wiiMappings = wiiMappingsOwned;
    wiiMappingCount = n;
}

static void applyAudioGain(void) {
    if (!gAudioSystem || !gAudioSystem->vtable || !gAudioSystem->vtable->setMasterGain) return;
    gAudioSystem->vtable->setMasterGain(gAudioSystem, gPortSettings.masterGain);
}

static int32_t findRoomIndexByName(DataWin* dw, const char* name) {
    if (!dw || !name || !name[0]) return -1;
    for (uint32_t i = 0; i < dw->room.count; i++) {
        Room* room = &dw->room.rooms[i];
        if (room->present && room->name && strcmp(room->name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

void platformLog(const logType type, const char* format, va_list va) {
    FILE* out = stderr;
    switch (type) {
        case LOG_TYPE_NORMAL:   out = stdout; break;
        case LOG_TYPE_WARNING:  fputs("Warning: ", stderr); break;
        case LOG_TYPE_ERROR:    fputs("Error: ",   stderr); break;
        case LOG_TYPE_DEBUG:    fputs("Debug: ",   stderr); break;
    }
    vfprintf(out, format, va);
}

static void parseWiiMappings(JsonValue* configRoot, const char* key) {
    JsonValue* obj = JsonReader_getJsonValueByKey(configRoot, key);
    if (obj == NULL || !JsonReader_isObject(obj)) return;
    int count = JsonReader_objectLength(obj);
    wiiMappings = (WiiMapping*)safeMalloc(sizeof(WiiMapping) * (size_t)count);
    wiiMappingCount = count;
    repeat(count, i) {
        const char* buttonStr = JsonReader_getJsonKeyByIndex(obj, i);
        JsonValue*  gmlKeyVal = JsonReader_getJsonValueByIndex(obj, i);
        wiiMappings[i].wpadButton = (uint32_t)atoi(buttonStr);
        wiiMappings[i].gmlKey    = (int32_t)JsonReader_getInt(gmlKeyVal);
        logInfo("CONFIG.JSN: Wii mapping wpad=0x%04X -> gmlKey=%d\n",
                wiiMappings[i].wpadButton, wiiMappings[i].gmlKey);
    }
}

static void installDefaultMappings(void) {
    static WiiMapping defaults[] = {
        { WPAD_BUTTON_LEFT,  VK_LEFT    },
        { WPAD_BUTTON_RIGHT, VK_RIGHT   },
        { WPAD_BUTTON_UP,    VK_UP      },
        { WPAD_BUTTON_DOWN,  VK_DOWN    },
        { WPAD_BUTTON_A,     'Z'        },
        { WPAD_BUTTON_B,     'X'        },
        { WPAD_BUTTON_PLUS,  VK_ENTER   },
        { WPAD_BUTTON_MINUS, VK_SHIFT   },
        { WPAD_BUTTON_1,     'C'        },
        { WPAD_BUTTON_2,     VK_ESCAPE  },
    };
    wiiMappings     = defaults;
    wiiMappingCount = (int)(sizeof(defaults) / sizeof(defaults[0]));
}

// Soft return to the Wii Channels / System Menu (not HBC, not a cold reboot).
static void returnToWiiMenu(void) {
    logInfo("Returning to Wii System Menu...\n");
    VIDEO_SetBlack(TRUE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
    // SYS_ResetSystem does not return on hardware; keep a fallback for odd hosts.
    while (1) {}
}

static void pollWpad(Runner* runner) {
    WPAD_ScanPads();
    uint32_t held = WPAD_ButtonsHeld(0);
    uint32_t down = WPAD_ButtonsDown(0);

    if (down & WPAD_BUTTON_HOME) {
        if (gAudioSystem && gAudioSystem->vtable && gAudioSystem->vtable->pauseAll) {
            gAudioSystem->vtable->pauseAll(gAudioSystem);
        }

        bool exitLoader = false;
        bool softReset = WiiSystemMenu_run(
            gRmode, gXfb0, gXfb1, gFbIndex, gBundleDir, &gPortSettings, runner, &exitLoader);
        applyPortMappings(&gPortSettings);
        applyAudioGain();
        WiiOverlay_setDebugOverlayState((WiiDebugOverlayState)gPortSettings.debugOverlay, runner);
        // Drop held keys so a held A from the menu doesn't leak into GML.
        prevHeld = 0;
        if (runner->keyboard) {
            for (int k = 0; k < GML_KEY_COUNT; k++) {
                if (runner->keyboard->keyDown[k]) {
                    RunnerKeyboard_onKeyUp(runner->keyboard, k);
                }
            }
        }

        if (exitLoader) {
            if (gAudioSystem && gAudioSystem->vtable && gAudioSystem->vtable->stopAll) {
                gAudioSystem->vtable->stopAll(gAudioSystem);
            }
            returnToWiiMenu();
            return;
        }
        if (softReset) {
            if (gAudioSystem && gAudioSystem->vtable && gAudioSystem->vtable->stopAll) {
                gAudioSystem->vtable->stopAll(gAudioSystem);
            }
            runner->pendingRoom = ROOM_RESTARTGAME;
            return;
        }

        if (gAudioSystem && gAudioSystem->vtable && gAudioSystem->vtable->resumeAll) {
            gAudioSystem->vtable->resumeAll(gAudioSystem);
        }
        return;
    }

    repeat(wiiMappingCount, i) {
        uint32_t mask   = wiiMappings[i].wpadButton;
        int32_t  gmlKey = wiiMappings[i].gmlKey;
        bool wasHeld = (prevHeld & mask) != 0;
        bool isHeld  = (held    & mask) != 0;
        if (isHeld && !wasHeld) RunnerKeyboard_onKeyDown(runner->keyboard, gmlKey);
        else if (!isHeld && wasHeld) RunnerKeyboard_onKeyUp(runner->keyboard, gmlKey);
    }
    prevHeld = held;
}

static void hangBlackScreen(void) {
    logError("Hanging with black screen.\n");
    while (1) {}
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // Route stdout/stderr to Dolphin's OSReport log window.
    SYS_STDIO_Report(true);

    // ===[ VIDEO + GX Init ]===
    VIDEO_Init();

    GXRModeObj* rmode = VIDEO_GetPreferredMode(NULL);

    void* xfb0 = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    void* xfb1 = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb0);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    void* gxFifo = memalign(32, GX_FIFO_SIZE);
    memset(gxFifo, 0, GX_FIFO_SIZE);
    GX_Init(gxFifo, GX_FIFO_SIZE);

    GXColor bgColor = {0, 0, 0, 255};
    GX_SetCopyClear(bgColor, GX_MAX_Z24);

    GX_SetViewport(0.0f, 0.0f, (f32)rmode->fbWidth, (f32)rmode->efbHeight, 0.0f, 1.0f);
    f32 yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
    u32 xfbHeight = GX_SetDispCopyYScale(yscale);
    GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopyDst(rmode->fbWidth, xfbHeight);
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
    GX_SetFieldMode(rmode->field_rendering,
                    ((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
    GX_SetPixelFmt(rmode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_CopyDisp(xfb0, GX_TRUE);
    GX_SetDispCopyGamma(GX_GM_1_0);

    // Heap budget baseline after XFBs/FIFO so MEM1/MEM2 used/total are meaningful.
    WiiOverlay_snapshotArenas();

    logInfo("Butterscotch Wii - VIDEO and GX initialized (%dx%d)\n",
            rmode->fbWidth, rmode->efbHeight);

    // ===[ FAT / SD Card ]===
    if (!fatInitDefault()) {
        logError("fatInitDefault() failed – no SD card?\n");
        hangBlackScreen();
    }

    // ===[ Locate data.win ]===
    static const char* const candidates[] = {
        "sd:/apps/butterscotch/data.win",
        "sd:/butterscotch/data.win",
    };
    const char* dataWinPath = NULL;
    const char* bundleDir   = NULL;
    for (int ci = 0; ci < 2; ci++) {
        FILE* f = fopen(candidates[ci], "rb");
        if (f) {
            fclose(f);
            dataWinPath = candidates[ci];
            bundleDir   = (ci == 0) ? "sd:/apps/butterscotch/" : "sd:/butterscotch/";
            break;
        }
    }
    if (dataWinPath == NULL) {
        logError("data.win not found on SD card.\n");
        hangBlackScreen();
    }
    logInfo("Found data.win at %s\n", dataWinPath);

    // ===[ Port settings + pre-boot shell ]===
    u32 fbIndex = 0;
    gRmode = rmode;
    gXfb0 = xfb0;
    gXfb1 = xfb1;
    gFbIndex = &fbIndex;
    gBundleDir = bundleDir;
    WiiSettings_load(&gPortSettings, bundleDir);
    {
        char saveDir[128];
        snprintf(saveDir, sizeof(saveDir), "%ssaves/", bundleDir);
        mkdir(saveDir, 0755);
    }
    if (WiiBootMenu_run(rmode, xfb0, xfb1, &fbIndex, bundleDir, &gPortSettings) == WII_BOOT_RETURN_TO_MENU) {
        returnToWiiMenu();
    }
    applyPortMappings(&gPortSettings);

    // ===[ Load CONFIG.JSN (optional) ]===
    char configPath[128];
    snprintf(configPath, sizeof(configPath), "%sCONFIG.JSN", bundleDir);
    JsonValue* configRoot  = NULL;
    bool       lazyLoadRooms = true;

    FILE* configFile = fopen(configPath, "rb");
    if (configFile) {
        fseek(configFile, 0, SEEK_END);
        long configSize = ftell(configFile);
        fseek(configFile, 0, SEEK_SET);
        char* configText = (char*)safeMalloc((size_t)configSize + 1);
        size_t n = fread(configText, 1, (size_t)configSize, configFile);
        configText[n] = '\0';
        fclose(configFile);
        configRoot = JsonReader_parse(configText);
        free(configText);
    }

    StringBooleanEntry* eagerRooms = NULL;
    JsonValue* disabledObjectsArr  = NULL;

    if (configRoot) {
        JsonValue* lazyVal = JsonReader_getJsonValueByKey(configRoot, "lazyLoadRooms");
        if (lazyVal) lazyLoadRooms = JsonReader_getBool(lazyVal);

        JsonValue* eagerArr = JsonReader_getJsonValueByKey(configRoot, "eagerlyLoadedRooms");
        if (eagerArr != NULL && JsonReader_isArray(eagerArr)) {
            int eagerCount = JsonReader_arrayLength(eagerArr);
            repeat(eagerCount, i) {
                const char* name = JsonReader_getString(JsonReader_getArrayElement(eagerArr, i));
                if (name) shput(eagerRooms, (char*)name, true);
            }
        }

        disabledObjectsArr = JsonReader_getJsonValueByKey(configRoot, "disabledObjects");

        // CONFIG.JSN mappings override port settings only if explicitly present.
        JsonValue* mapsObj = JsonReader_getJsonValueByKey(configRoot, "controller1Mappings");
        if (mapsObj != NULL && JsonReader_isObject(mapsObj)) {
            parseWiiMappings(configRoot, "controller1Mappings");
        }
    }

    if (wiiMappingCount == 0) {
        applyPortMappings(&gPortSettings);
    }

    // ===[ Parse data.win ]===
    DataWinParserOptions opts = {0};
    opts.parseGen8   = true;
    opts.parseOptn   = true;
    opts.parseLang   = true;
    opts.parseExtn   = true;
    opts.parseSond   = true;
    opts.parseAgrp   = true;
    opts.parseSprt   = true;
    opts.parseBgnd   = true;
    opts.parsePath   = true;
    opts.parseScpt   = true;
    opts.parseGlob   = true;
    opts.parseShdr   = true;
    opts.parseFont   = true;
    opts.parseTmln   = true;
    opts.parseObjt   = true;
    opts.parseRoom   = true;
    opts.parseTpag   = true;
    opts.parseCode   = true;
    opts.parseVari   = true;
    opts.parseFunc   = true;
    opts.parseStrg   = true;
    opts.parseTxtr   = true;
    opts.lazyLoadTextures = true;
    opts.parseAudo   = true;
    opts.lazyLoadAudio = true;
    opts.lazyLoadRooms = lazyLoadRooms;
    opts.eagerlyLoadedRooms = eagerRooms;
    opts.skipLoadingPreciseMasksForNonPreciseSprites = true;

    logInfo("Parsing data.win...\n");
    DataWin* dataWin = DataWin_parse(dataWinPath, opts);
    if (eagerRooms) shfree(eagerRooms);

    // ===[ Save path / OverlayFileSystem ]===
    char savePath[128];
    snprintf(savePath, sizeof(savePath), "%ssaves/", bundleDir);
    mkdir(savePath, 0755);

    OverlayFileSystem* ofs = OverlayFileSystem_create(bundleDir, savePath);
    FileSystem* fileSystem = (FileSystem*)ofs;

    // ===[ VM ]===
    logInfo("Creating VM...\n");
    VMContext* vm = VM_create(dataWin);

    // ===[ Renderer ]===
    logInfo("Creating renderer...\n");
    Renderer* renderer = GxRenderer_create(rmode, xfb0, xfb1, &fbIndex);

    // ===[ Audio ]===
#ifdef USE_WII_AUDIO
    logInfo("Creating Wii audio system...\n");
    WiiAudioSystem* wiiAudio = WiiAudioSystem_create();
    AudioSystem* audioSystem = (AudioSystem*)wiiAudio;
#else
    AudioSystem* audioSystem = (AudioSystem*)NoopAudioSystem_create();
#endif
    gAudioSystem = audioSystem;
    applyAudioGain();

    // ===[ Runner ]===
    logInfo("Creating runner...\n");
    Runner* runner = Runner_create(dataWin, vm, renderer, fileSystem, audioSystem);

    if (disabledObjectsArr && JsonReader_isArray(disabledObjectsArr)) {
        sh_new_strdup(runner->disabledObjects);
        int dc = JsonReader_arrayLength(disabledObjectsArr);
        repeat(dc, i) {
            JsonValue* elem = JsonReader_getArrayElement(disabledObjectsArr, i);
            if (elem && JsonReader_isString(elem)) {
                const char* objName = JsonReader_getString(elem);
                shput(runner->disabledObjects, objName, 1);
                logInfo("Disabled object: %s\n", objName);
            }
        }
    }

    // ===[ WPAD ]===
    // Boot menu already called WPAD_Init(); safe to call again.

    // ===[ First Room ]===
    logInfo("Initializing first room...\n");
    Runner_initFirstRoom(runner);
    if (gPortSettings.startRoomName[0]) {
        int32_t roomIdx = findRoomIndexByName(dataWin, gPortSettings.startRoomName);
        if (roomIdx >= 0) {
            logInfo("Extras: jumping to room %s (index %d)\n", gPortSettings.startRoomName, roomIdx);
            runner->pendingRoom = roomIdx;
            Runner_handlePendingRoomChange(runner);
        } else {
            logWarn("Extras: start room '%s' not found; using normal boot\n", gPortSettings.startRoomName);
        }
    }

    Gen8* gen8 = &dataWin->gen8;
    int32_t gameW = (int32_t)gen8->defaultWindowWidth;
    int32_t gameH = (int32_t)gen8->defaultWindowHeight;
    int32_t winW  = (int32_t)rmode->fbWidth;
    int32_t winH  = (int32_t)rmode->efbHeight;

    logInfo("Entering main loop (%dx%d game, %dx%d display)\n", gameW, gameH, winW, winH);

    WiiOverlay_init();
    WiiOverlay_setDebugOverlayState((WiiDebugOverlayState)gPortSettings.debugOverlay, runner);

    uint64_t lastTime = nowNanos();
    float lastPresentMs = 0.0f;
    uint32_t displayParity = 0;
    uint64_t lastStepTime = 0;
    float stepIntervalWindowMaxMs = 0.0f;
    float overlayStepMaxMs = 0.0f;
    int stepIntervalWindowFrames = 0;
    bool havePresented = false;

    // ===[ Main Loop ]===
    // Hybrid from A/B: 60draw present cadence + 30draw Draw timing.
    // Non-step VI: EFB→XFB duplicate BEFORE any GX that can clobber the EFB
    // (tex pump used to run first and wipe the held frame). Audio update is
    // first every VI so stream refill is not stuck behind pump/draw.
    while (!runner->shouldExit) {
        uint64_t frameStart = nowNanos();
        uint64_t deltaNs  = frameStart - lastTime;
        lastTime          = frameStart;

        uint32_t roomSpeed = runner->currentRoom->speed;
        bool stepThisFrame = (roomSpeed > 35) || ((displayParity & 1u) == 0u);

        pollWpad(runner);

        // Wall-clock dt for APIs that care; stream refill ignores it but keep honest.
        float dt = (float)((double)deltaNs / 1e9);
        if (dt <= 0.0f || dt > 0.1f) {
            dt = (roomSpeed > 0) ? (1.0f / (float)roomSpeed) : (1.0f / 30.0f);
        }

        uint64_t audioStart = nowNanos();
        audioSystem->vtable->update(audioSystem, dt);
        uint64_t audioEnd = nowNanos();

        // Hold EFB→XFB copy must happen before tex pump / draw touch GX.
        if (!stepThisFrame && havePresented) {
            uint64_t presentStart = nowNanos();
            GxRenderer_presentDuplicate(renderer);
            lastPresentMs = (float)((double)(nowNanos() - presentStart) / 1e6);
        }

        GxRenderer_pumpTexLoads(renderer, 2ull * 1000ull * 1000ull);

        float stepMs = 0.0f;
        float drawMs = 0.0f;
        float tickMs = (float)((double)(audioEnd - frameStart) / 1e6);

        if (stepThisFrame) {
            if (lastStepTime != 0) {
                float intervalMs = (float)((double)(frameStart - lastStepTime) / 1e6);
                if (intervalMs > stepIntervalWindowMaxMs) stepIntervalWindowMaxMs = intervalMs;
                runner->deltaTime = (double)(frameStart - lastStepTime) / 1000.0;
            } else {
                runner->deltaTime = (double)deltaNs / 1000.0;
            }
            lastStepTime = frameStart;

            uint64_t stepStart = nowNanos();
            Runner_step(runner);
            uint64_t stepEnd = nowNanos();
            stepMs = (float)((double)(stepEnd - stepStart) / 1e6);

            Runner_drawPre(runner, winW, winH);
            Runner_beginFrame(runner, gameW, gameH, winW, winH, winW, winH);

            uint64_t drawStart = nowNanos();
            Runner_drawViews(runner, gameW, gameH, false);
            runner->viewCurrent = 0;
            renderer->vtable->endFrameInit(renderer);
            Runner_drawPost(runner, winW, winH);
            renderer->vtable->endFrameEnd(renderer);
            Runner_drawGUI(runner, winW, winH, gameW, gameH);
            uint64_t drawEnd = nowNanos();
            drawMs = (float)((double)(drawEnd - drawStart) / 1e6);

            RunnerKeyboard_beginFrame(runner->keyboard);

            stepIntervalWindowFrames++;
            if (stepIntervalWindowFrames >= 30) {
                overlayStepMaxMs = stepIntervalWindowMaxMs;
                stepIntervalWindowMaxMs = 0.0f;
                stepIntervalWindowFrames = 0;
            }

            tickMs = (float)((double)(drawEnd - frameStart) / 1e6);
            float audioMs = (float)((double)(audioEnd - audioStart) / 1e6);
            float stepIntervalMs = (float)(runner->deltaTime / 1000.0);
            if (stepIntervalMs < 1.0f) stepIntervalMs = 33.333f;
            WiiOverlay_drawDebugOverlay(renderer, runner, tickMs, stepMs, drawMs, audioMs,
                                        lastPresentMs, stepIntervalMs, overlayStepMaxMs,
                                        winW, winH);

            uint64_t presentStart = nowNanos();
            GxRenderer_present(renderer, false);
            lastPresentMs = (float)((double)(nowNanos() - presentStart) / 1e6);
            havePresented = true;

            Runner_handlePendingRoomChange(runner);
        }

        VIDEO_WaitVSync();
        displayParity++;
    }

    // Undertale's Hold-ESC path calls game_end → shouldExit. A bare return from
    // main in a Wii DOL hard-crashes Dolphin and can PPC-halt on hardware.
    // Treat in-game quit the same as the boot/system menu "Return to Wii Menu".
    if (gAudioSystem && gAudioSystem->vtable && gAudioSystem->vtable->stopAll) {
        gAudioSystem->vtable->stopAll(gAudioSystem);
    }
    WiiOverlay_deinit();
    returnToWiiMenu();
    return 0;
}
