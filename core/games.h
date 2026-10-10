/* Shared, bounded per-title policy for the Shell plugin and settings app. */
#ifndef VJO_GAMES_H
#define VJO_GAMES_H
#include <stddef.h>
#define VJO_GAMES_MAX 512
#define VJO_TITLE_ID_SIZE 12
#define VJO_GAMES_TEXT_MAX (12 * 1024)
#define VJO_SETTINGS_TITLE_ID "VJOSET001"
typedef struct {
    char title_id[VJO_TITLE_ID_SIZE];
    int enabled;
} VjoGameRule;
typedef struct {
    int default_enabled;
    int count;
    VjoGameRule rules[VJO_GAMES_MAX];
} VjoGames;
int vjo_game_id_valid(const char *id);
/* Adrenaline is allowed: its PSP games share NPXS10028. */
int vjo_game_is_candidate(const char *id);
void vjo_games_defaults(VjoGames *games);
int vjo_games_enabled(const VjoGames *games, const char *id);
int vjo_games_set(VjoGames *games, const char *id, int enabled);
/* Strict parsing: callers must not use partially parsed data on error. */
int vjo_games_parse(VjoGames *games, const char *text, size_t len);
int vjo_games_format(const VjoGames *games, char *out, size_t cap);
#endif
