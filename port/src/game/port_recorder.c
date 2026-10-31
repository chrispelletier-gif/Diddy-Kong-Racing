/*
 * The game's side of the flight recorder (plat/recorder.h): what the game is doing, as numbers in every frame's row
 * and in words for a bug report. Called on the game thread only, at the end of a frame. Not in a player's build.
 */
#ifndef PORT_RELEASE
#include <ultra64.h>
#include "types.h"
#include "structs.h"
#include "game.h"
#include "menu.h"
#include "objects.h"
#include "thread3_main.h"
#include "port.h"
#include "plat/plat.h"
#include "plat/recorder.h"

extern s32 gCurrentMenuId;
extern f32 gObjectUpdateRateF;

static const char *const sMenuNames[] = {
    "title",          "logos",          "unused 2",          "character select", "unused 4",
    "adventure track select", "file select", "unused 7",     "unused 8",         "unused 9",
    "magic codes",    "magic codes list", "options",         "audio options",    "save options",
    "track select",   "unused 16",      "results",           "unused 18",        "game select",
    "trophy race round", "trophy race rankings", "unused 22", "new game cinematic", "ghost data",
    "credits",        "boot",           "unused 27",         "caution",
};

static const char *const sCharacterNames[] = {
    "Krunch", "Bumper", "Tiptup", "Conker", "Timber", "Banjo", "Drumstick", "Pipsy", "T.T.", "Diddy",
};

static const char *const sVehicleNames[] = { "car", "hovercraft", "plane" };

void port_rec_game_state(RecFrame *row) {
    PlatGameNote note;
    row->mode = (signed char) get_game_mode();
    row->level = (signed char) level_id();
    row->menu = (signed char) gCurrentMenuId;
    row->paused = is_game_paused() != 0;
    if (plat_get_game_note(&note) && note.racing) {
        row->racing = 1;
        row->lap = (unsigned char) note.lap;
        row->checkpoint = (unsigned char) note.checkpoint;
    }
}

void port_rec_describe(char *out, int size) {
    s32 mode = get_game_mode(), level = level_id();
    const char *levelName = level_name(level);
    const char *menuName = (gCurrentMenuId >= 0 && gCurrentMenuId < (s32) ARRAY_COUNT(sMenuNames))
                               ? sMenuNames[gCurrentMenuId]
                               : "?";
    Object *obj = (mode == GAMEMODE_INGAME) ? get_racer_object_by_port(PLAYER_ONE) : NULL;
    Object_Racer *racer = (obj != NULL) ? obj->racer : NULL;
    char kart[160];
    PlatGameNote note;

    kart[0] = '\0';
    if (racer != NULL) {
        char noteText[96];
        noteText[0] = '\0';
        if (plat_get_game_note(&note)) {
            plat_format_game_note(&note, noteText, sizeof(noteText));
        }
        plat_snprintf(kart, sizeof(kart), "; player %s in a %s, %s",
                      racer->characterId >= 0 && racer->characterId < (s32) ARRAY_COUNT(sCharacterNames)
                          ? sCharacterNames[(s32) racer->characterId]
                          : "?",
                      racer->vehicleID >= 0 && racer->vehicleID < (s32) ARRAY_COUNT(sVehicleNames)
                          ? sVehicleNames[(s32) racer->vehicleID]
                          : "vehicle (boss or special)",
                      noteText);
    }
    plat_snprintf(out, size, "%s, level %d (%s)%s%s%s, logic rate %d, game thread %s%s",
                  mode == GAMEMODE_INTRO    ? "intro"
                  : mode == GAMEMODE_INGAME ? "in game"
                  : mode == GAMEMODE_MENU   ? "in a menu"
                  : mode == GAMEMODE_LOCKUP ? "lock-up screen"
                                            : "other mode",
                  (int) level, levelName != NULL ? levelName : "no level",
                  mode == GAMEMODE_MENU ? ", menu " : "", mode == GAMEMODE_MENU ? menuName : "",
                  is_game_paused() ? ", paused" : "", (int) gObjectUpdateRateF,
                  gPortGameStage != NULL ? gPortGameStage : "-", kart);
}
#endif
