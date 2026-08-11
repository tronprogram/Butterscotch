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
#include "../utils.h"
#include "../gettime.h"

#ifdef USE_WII_AUDIO
#include "wii_audio_system.h"
#endif

#define GX_FIFO_SIZE (256 * 1024)

// Button to GML key mapping
typedef struct {
    uint32_t wpadButton;
    int32_t  gmlKey;
} WiiMapping;

static WiiMapping* wiiMappings  = NULL;
static int         wiiMappingCount = 0;

static uint32_t prevHeld = 0;

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

static void pollWpad(Runner* runner) {
    WPAD_ScanPads();
    uint32_t held = WPAD_ButtonsHeld(0);

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

        JsonValue* mapsObj = JsonReader_getJsonValueByKey(configRoot, "controller1Mappings");
        if (mapsObj != NULL && JsonReader_isObject(mapsObj)) {
            parseWiiMappings(configRoot, "controller1Mappings");
        }
    }

    if (wiiMappingCount == 0) {
        installDefaultMappings();
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
    u32 fbIndex = 0;
    Renderer* renderer = GxRenderer_create(rmode, xfb0, xfb1, &fbIndex);

    // ===[ Audio ]===
#ifdef USE_WII_AUDIO
    logInfo("Creating Wii audio system...\n");
    WiiAudioSystem* wiiAudio = WiiAudioSystem_create();
    AudioSystem* audioSystem = (AudioSystem*)wiiAudio;
#else
    AudioSystem* audioSystem = (AudioSystem*)NoopAudioSystem_create();
#endif

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
    WPAD_Init();

    // ===[ First Room ]===
    logInfo("Initializing first room...\n");
    Runner_initFirstRoom(runner);

    Gen8* gen8 = &dataWin->gen8;
    int32_t gameW = (int32_t)gen8->defaultWindowWidth;
    int32_t gameH = (int32_t)gen8->defaultWindowHeight;
    int32_t winW  = (int32_t)rmode->fbWidth;
    int32_t winH  = (int32_t)rmode->efbHeight;

    logInfo("Entering main loop (%dx%d game, %dx%d display)\n", gameW, gameH, winW, winH);

    uint64_t lastTime = nowNanos();

    // ===[ Main Loop ]===
    while (!runner->shouldExit) {
        uint64_t now      = nowNanos();
        uint64_t deltaNs  = now - lastTime;
        lastTime          = now;
        runner->deltaTime = (double)deltaNs / 1000.0;

        pollWpad(runner);

        Runner_step(runner);

        Runner_drawPre(runner, winW, winH);
        Runner_beginFrame(runner, gameW, gameH, winW, winH, winW, winH);
        Runner_drawViews(runner, gameW, gameH, false);
        runner->viewCurrent = 0;
        renderer->vtable->endFrameInit(renderer);
        Runner_drawPost(runner, winW, winH);
        renderer->vtable->endFrameEnd(renderer);
        Runner_drawGUI(runner, winW, winH, gameW, gameH);

        RunnerKeyboard_beginFrame(runner->keyboard);

        uint32_t roomSpeed = runner->currentRoom->speed;
        float dt = (roomSpeed > 0) ? (1.0f / (float)roomSpeed) : (1.0f / 30.0f);
        if (dt > 0.1f) dt = 0.1f;
        audioSystem->vtable->update(audioSystem, dt);

        GxRenderer_present(renderer);

        Runner_handlePendingRoomChange(runner);

        if (roomSpeed > 0) {
            uint64_t targetNs = (uint64_t)(1000000000ULL / roomSpeed);
            while ((nowNanos() - lastTime) < targetNs) {}
        }
    }

    audioSystem->vtable->destroy(audioSystem);
    renderer->vtable->destroy(renderer);
    DataWin_free(dataWin);

    return 0;
}
