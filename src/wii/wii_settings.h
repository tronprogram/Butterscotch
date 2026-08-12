#ifndef _BS_WII_SETTINGS_H_
#define _BS_WII_SETTINGS_H_

#include <stdbool.h>
#include <stdint.h>

#define WII_SETTINGS_MAP_MAX 16
#define WII_SETTINGS_ROOM_NAME_MAX 64

/* GameCube PAD_* masks are stored with this high bit so they don't collide with WPAD. */
#define WII_INPUT_GC_FLAG 0x40000000u
#define WII_INPUT_IS_GC(mask) (((mask) & WII_INPUT_GC_FLAG) != 0)
#define WII_INPUT_GC_PAD(mask) ((uint16_t)((mask) & ~WII_INPUT_GC_FLAG))

typedef enum {
    WII_CTRL_PRESET_WIIMOTE_VERT = 0,
    WII_CTRL_PRESET_WIIMOTE_HORIZ = 1,
    WII_CTRL_PRESET_GAMECUBE = 2,
    WII_CTRL_PRESET_CLASSIC = 3,
    WII_CTRL_PRESET_COUNT = 4,
} WiiControllerPreset;

typedef struct {
    uint32_t wpadButton; /* WPAD_* / WPAD_CLASSIC_* or (WII_INPUT_GC_FLAG | PAD_*) */
    int32_t gmlKey;
} WiiSettingsMapping;

typedef struct {
    float masterGain; // 0..1
    // Matches WiiDebugOverlayState: 0=on, 1=profiler, 2=off
    int debugOverlay;
    // Empty => normal Undertale boot. Else room asset name (e.g. "room_castle_barrier").
    char startRoomName[WII_SETTINGS_ROOM_NAME_MAX];
    int controllerPreset; // WiiControllerPreset
    int mapCount;
    WiiSettingsMapping maps[WII_SETTINGS_MAP_MAX];
} WiiPortSettings;

void WiiSettings_setDefaults(WiiPortSettings* s);
// bundleDir e.g. "sd:/apps/butterscotch/" — loads saves/wii_settings.json if present.
bool WiiSettings_load(WiiPortSettings* s, const char* bundleDir);
bool WiiSettings_save(const WiiPortSettings* s, const char* bundleDir);
void WiiSettings_installDefaultMaps(WiiPortSettings* s);
void WiiSettings_applyPreset(WiiPortSettings* s, int preset);
const char* WiiSettings_presetName(int preset);

#endif /* _BS_WII_SETTINGS_H_ */
