#ifndef _BS_WII_GAMES_H_
#define _BS_WII_GAMES_H_

#define WII_GAME_ID_MAX 32
#define WII_GAME_TITLE_MAX 64
#define WII_GAME_PATH_MAX 160
#define WII_GAMES_MAX 16

typedef struct {
    char id[WII_GAME_ID_MAX];
    char title[WII_GAME_TITLE_MAX];
    char path[WII_GAME_PATH_MAX]; // sd:/apps/butterscotch/games/ut/
} WiiGameEntry;

// Scan engineRoot/games/<id>/data.win. If none, fall back to engineRoot/data.win.
// engineRoot e.g. "sd:/apps/butterscotch" (trailing slash optional).
int WiiGames_scan(const char* engineRoot, WiiGameEntry* out, int maxEntries);

#endif /* _BS_WII_GAMES_H_ */
