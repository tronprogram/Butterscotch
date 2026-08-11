#ifndef _BS_WII_SETTINGS_H_
#define _BS_WII_SETTINGS_H_

#include <stdbool.h>
#include <stdint.h>

#define WII_SETTINGS_MAP_MAX 16
#define WII_SETTINGS_ROOM_NAME_MAX 64

typedef struct {
    uint32_t wpadButton;
    int32_t gmlKey;
} WiiSettingsMapping;

typedef struct {
    float masterGain; // 0..1
    // Matches WiiDebugOverlayState: 0=on, 1=profiler, 2=off
    int debugOverlay;
    // Empty => normal Undertale boot. Else room asset name (e.g. "room_castle_barrier").
    char startRoomName[WII_SETTINGS_ROOM_NAME_MAX];
    int mapCount;
    WiiSettingsMapping maps[WII_SETTINGS_MAP_MAX];
} WiiPortSettings;

void WiiSettings_setDefaults(WiiPortSettings* s);
// bundleDir e.g. "sd:/apps/butterscotch/" — loads saves/wii_settings.json if present.
bool WiiSettings_load(WiiPortSettings* s, const char* bundleDir);
bool WiiSettings_save(const WiiPortSettings* s, const char* bundleDir);
void WiiSettings_installDefaultMaps(WiiPortSettings* s);

#endif /* _BS_WII_SETTINGS_H_ */
