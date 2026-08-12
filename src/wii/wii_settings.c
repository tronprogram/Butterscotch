#include "wii_settings.h"

#include "../json_reader.h"
#include "../json_writer.h"
#include "../utils.h"

#include <gccore.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <wiiuse/wpad.h>

#include "runner_keyboard.h"

static void copyMaps(WiiPortSettings* s, const WiiSettingsMapping* src, int n) {
    if (n > WII_SETTINGS_MAP_MAX) n = WII_SETTINGS_MAP_MAX;
    s->mapCount = n;
    memcpy(s->maps, src, (size_t)n * sizeof(WiiSettingsMapping));
}

void WiiSettings_applyPreset(WiiPortSettings* s, int preset) {
    if (preset < 0 || preset >= WII_CTRL_PRESET_COUNT) {
        preset = WII_CTRL_PRESET_WIIMOTE_VERT;
    }
    s->controllerPreset = preset;

    switch ((WiiControllerPreset)preset) {
        case WII_CTRL_PRESET_WIIMOTE_VERT: {
            /* Remote upright — current Butterscotch default. */
            static const WiiSettingsMapping maps[] = {
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
            copyMaps(s, maps, (int)(sizeof(maps) / sizeof(maps[0])));
            break;
        }
        case WII_CTRL_PRESET_WIIMOTE_HORIZ: {
            /* Sideways (tip right): rotate D-pad; 2/1 as confirm/cancel. */
            static const WiiSettingsMapping maps[] = {
                { WPAD_BUTTON_UP,    VK_LEFT    },
                { WPAD_BUTTON_DOWN,  VK_RIGHT   },
                { WPAD_BUTTON_LEFT,  VK_DOWN    },
                { WPAD_BUTTON_RIGHT, VK_UP      },
                { WPAD_BUTTON_2,     'Z'        },
                { WPAD_BUTTON_1,     'X'        },
                { WPAD_BUTTON_A,     'C'        },
                { WPAD_BUTTON_B,     VK_SHIFT   },
                { WPAD_BUTTON_PLUS,  VK_ENTER   },
                { WPAD_BUTTON_MINUS, VK_ESCAPE  },
            };
            copyMaps(s, maps, (int)(sizeof(maps) / sizeof(maps[0])));
            break;
        }
        case WII_CTRL_PRESET_GAMECUBE: {
            static const WiiSettingsMapping maps[] = {
                { WII_INPUT_GC_FLAG | PAD_BUTTON_LEFT,  VK_LEFT    },
                { WII_INPUT_GC_FLAG | PAD_BUTTON_RIGHT, VK_RIGHT   },
                { WII_INPUT_GC_FLAG | PAD_BUTTON_UP,    VK_UP      },
                { WII_INPUT_GC_FLAG | PAD_BUTTON_DOWN,  VK_DOWN    },
                { WII_INPUT_GC_FLAG | PAD_BUTTON_A,     'Z'        },
                { WII_INPUT_GC_FLAG | PAD_BUTTON_B,     'X'        },
                { WII_INPUT_GC_FLAG | PAD_BUTTON_X,     'C'        },
                { WII_INPUT_GC_FLAG | PAD_BUTTON_Y,     VK_SHIFT   },
                { WII_INPUT_GC_FLAG | PAD_BUTTON_START, VK_ENTER   },
                { WII_INPUT_GC_FLAG | PAD_TRIGGER_Z,    VK_ESCAPE  },
            };
            copyMaps(s, maps, (int)(sizeof(maps) / sizeof(maps[0])));
            break;
        }
        case WII_CTRL_PRESET_CLASSIC: {
            static const WiiSettingsMapping maps[] = {
                { WPAD_CLASSIC_BUTTON_LEFT,  VK_LEFT    },
                { WPAD_CLASSIC_BUTTON_RIGHT, VK_RIGHT   },
                { WPAD_CLASSIC_BUTTON_UP,    VK_UP      },
                { WPAD_CLASSIC_BUTTON_DOWN,  VK_DOWN    },
                { WPAD_CLASSIC_BUTTON_A,     'Z'        },
                { WPAD_CLASSIC_BUTTON_B,     'X'        },
                { WPAD_CLASSIC_BUTTON_X,     'C'        },
                { WPAD_CLASSIC_BUTTON_Y,     VK_SHIFT   },
                { WPAD_CLASSIC_BUTTON_PLUS,  VK_ENTER   },
                { WPAD_CLASSIC_BUTTON_MINUS, VK_ESCAPE  },
            };
            copyMaps(s, maps, (int)(sizeof(maps) / sizeof(maps[0])));
            break;
        }
        default:
            break;
    }
}

const char* WiiSettings_presetName(int preset) {
    switch ((WiiControllerPreset)preset) {
        case WII_CTRL_PRESET_WIIMOTE_VERT:  return "Wii Remote (Vertical)";
        case WII_CTRL_PRESET_WIIMOTE_HORIZ: return "Wii Remote (Horizontal)";
        case WII_CTRL_PRESET_GAMECUBE:      return "GameCube Controller";
        case WII_CTRL_PRESET_CLASSIC:       return "Classic Controller";
        default:                            return "Unknown";
    }
}

void WiiSettings_installDefaultMaps(WiiPortSettings* s) {
    WiiSettings_applyPreset(s, WII_CTRL_PRESET_WIIMOTE_VERT);
}

void WiiSettings_setDefaults(WiiPortSettings* s) {
    memset(s, 0, sizeof(*s));
    s->masterGain = 1.0f;
    s->debugOverlay = 0; // stats on
    s->startRoomName[0] = '\0';
    WiiSettings_applyPreset(s, WII_CTRL_PRESET_WIIMOTE_VERT);
}

static void settingsPath(char* out, size_t outSz, const char* bundleDir) {
    snprintf(out, outSz, "%ssaves/wii_settings.json", bundleDir);
}

bool WiiSettings_load(WiiPortSettings* s, const char* bundleDir) {
    WiiSettings_setDefaults(s);
    char path[192];
    settingsPath(path, sizeof(path), bundleDir);
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024) {
        fclose(f);
        return false;
    }
    char* text = (char*)safeMalloc((size_t)sz + 1);
    size_t n = fread(text, 1, (size_t)sz, f);
    text[n] = '\0';
    fclose(f);

    JsonValue* root = JsonReader_parse(text);
    free(text);
    if (!root || !JsonReader_isObject(root)) {
        if (root) JsonReader_free(root);
        return false;
    }

    JsonValue* v;
    v = JsonReader_getJsonValueByKey(root, "masterGain");
    if (v && JsonReader_isNumber(v)) {
        float g = (float)JsonReader_getDouble(v);
        if (g < 0.0f) g = 0.0f;
        if (g > 1.0f) g = 1.0f;
        s->masterGain = g;
    }
    v = JsonReader_getJsonValueByKey(root, "debugOverlay");
    if (v && JsonReader_isNumber(v)) {
        int d = (int)JsonReader_getInt(v);
        if (d < 0) d = 0;
        if (d > 2) d = 2;
        s->debugOverlay = d;
    }
    v = JsonReader_getJsonValueByKey(root, "startRoomName");
    if (v && JsonReader_isString(v)) {
        const char* name = JsonReader_getString(v);
        if (name) {
            strncpy(s->startRoomName, name, sizeof(s->startRoomName) - 1);
            s->startRoomName[sizeof(s->startRoomName) - 1] = '\0';
        }
    }
    v = JsonReader_getJsonValueByKey(root, "controllerPreset");
    if (v && JsonReader_isNumber(v)) {
        int p = (int)JsonReader_getInt(v);
        if (p < 0) p = 0;
        if (p >= WII_CTRL_PRESET_COUNT) p = WII_CTRL_PRESET_COUNT - 1;
        s->controllerPreset = p;
    }
    v = JsonReader_getJsonValueByKey(root, "controllerMappings");
    if (v && JsonReader_isArray(v)) {
        int nMaps = JsonReader_arrayLength(v);
        if (nMaps > WII_SETTINGS_MAP_MAX) nMaps = WII_SETTINGS_MAP_MAX;
        int wrote = 0;
        for (int i = 0; i < nMaps; i++) {
            JsonValue* e = JsonReader_getArrayElement(v, i);
            if (!e || !JsonReader_isObject(e)) continue;
            JsonValue* wb = JsonReader_getJsonValueByKey(e, "wpad");
            JsonValue* gk = JsonReader_getJsonValueByKey(e, "gmlKey");
            if (!wb || !gk || !JsonReader_isNumber(wb) || !JsonReader_isNumber(gk)) continue;
            s->maps[wrote].wpadButton = (uint32_t)JsonReader_getInt(wb);
            s->maps[wrote].gmlKey = (int32_t)JsonReader_getInt(gk);
            wrote++;
        }
        if (wrote > 0) {
            s->mapCount = wrote;
        } else {
            WiiSettings_applyPreset(s, s->controllerPreset);
        }
    } else {
        WiiSettings_applyPreset(s, s->controllerPreset);
    }

    JsonReader_free(root);
    return true;
}

bool WiiSettings_save(const WiiPortSettings* s, const char* bundleDir) {
    char dir[192];
    snprintf(dir, sizeof(dir), "%ssaves/", bundleDir);
    mkdir(dir, 0755);

    char path[192];
    settingsPath(path, sizeof(path), bundleDir);

    JsonWriter w = JsonWriter_create();
    JsonWriter_beginObject(&w);
    JsonWriter_propertyDouble(&w, "masterGain", (double)s->masterGain);
    JsonWriter_propertyInt(&w, "debugOverlay", s->debugOverlay);
    JsonWriter_propertyString(&w, "startRoomName", s->startRoomName);
    JsonWriter_propertyInt(&w, "controllerPreset", s->controllerPreset);
    JsonWriter_key(&w, "controllerMappings");
    JsonWriter_beginArray(&w);
    for (int i = 0; i < s->mapCount; i++) {
        JsonWriter_beginObject(&w);
        JsonWriter_propertyInt(&w, "wpad", (int64_t)s->maps[i].wpadButton);
        JsonWriter_propertyInt(&w, "gmlKey", s->maps[i].gmlKey);
        JsonWriter_endObject(&w);
    }
    JsonWriter_endArray(&w);
    JsonWriter_endObject(&w);

    FILE* f = fopen(path, "wb");
    if (!f) {
        JsonWriter_free(&w);
        return false;
    }
    const char* out = JsonWriter_getOutput(&w);
    size_t len = JsonWriter_getLength(&w);
    bool ok = fwrite(out, 1, len, f) == len;
    fclose(f);
    JsonWriter_free(&w);
    return ok;
}
