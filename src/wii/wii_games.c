#include "wii_games.h"

#include "json_reader.h"
#include "stdio_compat.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void trimSlash(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '/' || s[n - 1] == '\\')) {
        s[--n] = '\0';
    }
}

static int fileExists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void applyGameJson(WiiGameEntry* e) {
    char jsonPath[WII_GAME_PATH_MAX + 16];
    snprintf(jsonPath, sizeof(jsonPath), "%sgame.json", e->path);
    FILE* f = fopen(jsonPath, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4096) {
        fclose(f);
        return;
    }
    char* text = (char*)malloc((size_t)sz + 1);
    if (!text) {
        fclose(f);
        return;
    }
    size_t n = fread(text, 1, (size_t)sz, f);
    text[n] = '\0';
    fclose(f);
    JsonValue* root = JsonReader_parse(text);
    free(text);
    if (!root) return;
    JsonValue* title = JsonReader_getJsonValueByKey(root, "title");
    if (title && JsonReader_isString(title)) {
        const char* s = JsonReader_getString(title);
        if (s && s[0]) {
            strncpy(e->title, s, sizeof(e->title) - 1);
            e->title[sizeof(e->title) - 1] = '\0';
        }
    }
    JsonReader_free(root);
}

static int addGame(WiiGameEntry* out, int* count, int max,
                   const char* id, const char* title, const char* dirWithSlash) {
    if (*count >= max) return 0;
    WiiGameEntry* e = &out[*count];
    memset(e, 0, sizeof(*e));
    strncpy(e->id, id, sizeof(e->id) - 1);
    strncpy(e->title, title, sizeof(e->title) - 1);
    strncpy(e->path, dirWithSlash, sizeof(e->path) - 1);
    applyGameJson(e);
    (*count)++;
    return 1;
}

int WiiGames_scan(const char* engineRoot, WiiGameEntry* out, int maxEntries) {
    if (!engineRoot || !out || maxEntries <= 0) return 0;
    char root[WII_GAME_PATH_MAX];
    strncpy(root, engineRoot, sizeof(root) - 1);
    root[sizeof(root) - 1] = '\0';
    trimSlash(root);

    int count = 0;
    char gamesDir[WII_GAME_PATH_MAX];
    snprintf(gamesDir, sizeof(gamesDir), "%s/games", root);

    DIR* d = opendir(gamesDir);
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char dir[WII_GAME_PATH_MAX];
            char dataWin[WII_GAME_PATH_MAX + 16];
            snprintf(dir, sizeof(dir), "%s/%s/", gamesDir, ent->d_name);
            snprintf(dataWin, sizeof(dataWin), "%sdata.win", dir);
            if (!fileExists(dataWin)) continue;
            addGame(out, &count, maxEntries, ent->d_name, ent->d_name, dir);
        }
        closedir(d);
    }

    if (count == 0) {
        char legacy[WII_GAME_PATH_MAX + 16];
        snprintf(legacy, sizeof(legacy), "%s/data.win", root);
        if (fileExists(legacy)) {
            char dir[WII_GAME_PATH_MAX];
            snprintf(dir, sizeof(dir), "%s/", root);
            addGame(out, &count, maxEntries, "default", "Butterscotch", dir);
        }
    }
    return count;
}
