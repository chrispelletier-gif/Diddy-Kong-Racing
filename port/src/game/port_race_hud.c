/*
 * The race HUD on the 3DS bottom screen (roadmap G9): once a frame (src/thread3_main.c), during a one player race,
 * this copies what the HUD shows (position, lap, times, item, bananas) and where every racer is on the track's map
 * into a RaceHud (plat/race_hud.h) for the bottom screen to draw, and works out the time gaps between the racers.
 *
 * A gap is measured at the track's checkpoints: the race clock is noted as each racer first reaches each one, and the
 * gap to another racer is the difference at the newest checkpoint both have reached, as race broadcasts do.
 *
 * Where the HUD goes is a setting, racehud= in settings.txt (DKR_RACE_HUD in env.txt for a test): 1 the bottom screen
 * (the 3DS's default; the top screen keeps only the countdown, lap announcements, wrong way, direction arrows and the
 * finish), 2 both screens, 0 the top screen only, as on the N64 (the default on PC, which has no bottom screen).
 * src/game_ui.c asks port_race_hud_hides() which of its pieces to leave out.
 */
#include <ultra64.h>
#include "types.h"
#include "structs.h"
#include "game.h"
#include "game_ui.h"
#include "menu.h"
#include "objects.h"
#include "thread3_main.h"
#include "tracks.h"
#include "math_util.h"
#include "port.h"
#include "plat/plat.h"
#include "plat/race_hud.h"
#include "plat/settings.h"
#include "asset_enums.h"
#include "textures_sprites.h"
#include <stdlib.h>

/* The bottom screen's side (plat_3ds.c; on PC bottom_preview.c draws it for DKR_BOTTOM_RACE). */
void plat_set_race_hud(const RaceHud *hud);
/* Whether there is a bottom screen to show the race HUD on. */
int plat_race_hud_screen(void);
int plat_race_hud_covered(void);
void plat_set_bottom_style(int style);

extern s32 gMinimapDotOffsetX;
extern s16 gTTGhostTimeToBeat; // T.T.'s time on this track (objects.c), while his ghost is out.

/* How many checkpoints a racer's gaps can be timed at: (laps + 3) * checkpoints is the most a race counts. */
#define MAX_STAMPS 768

static volatile s32 sSetting = -1; // RACE_HUD_*, -1 until loaded.
static s32 sActive;                // The bottom screen is showing this race's HUD.
static s32 sMenuShown;             // It is showing a menu's companion (menu_companion).
static s32 sMapSprite = -1;        // The current track's map, as minimap_init loaded it.
static void *sMapModel;            // The level model it belongs to: no map until the new level's is loaded.
static s32 sClock;                 // 60ths of a second since the start.
static s32 sTracking, sTrackedMode; // In a race (of this RACE_HUD_MODE_), keeping the clock and checkpoints.
static s16 sReached[RACE_HUD_RACERS];
static s32 sStamps[RACE_HUD_RACERS][MAX_STAMPS];

enum { RACE_HUD_TOP, RACE_HUD_BOTTOM, RACE_HUD_BOTH };

s32 port_race_hud_setting(void) {
    if (sSetting < 0) {
        const char *env = plat_getenv("DKR_RACE_HUD");
        // Without a bottom screen (PC, unless DKR_BOTTOM_RACE wants its pictures) the N64's HUD.
        s32 fallback = plat_race_hud_screen() ? RACE_HUD_BOTTOM : RACE_HUD_TOP;
        s32 value = env != NULL ? atoi(env) : settings_get("racehud", fallback);
        sSetting = (value < RACE_HUD_TOP || value > RACE_HUD_BOTH) ? fallback : value;
    }
    return sSetting;
}

void port_set_race_hud_setting(s32 value) {
    sSetting = value;
    settings_set("racehud", value);
}

/* Called by minimap_init (src/game_ui.c) with the track's map sprite, before it is loaded over the index. */
void port_race_hud_map(void *model, s32 spriteIndex) {
    sMapModel = model;
    sMapSprite = spriteIndex;
}

/* Whether game_ui.c leaves this HUD element (HUD_*, or -1 for the track map itself) off the top screen. */
s32 port_race_hud_hides(s32 element) {
    // While the bottom screen's menu covers the race HUD, the top screen keeps its own.
    if (!sActive || port_race_hud_setting() != RACE_HUD_BOTTOM || plat_race_hud_covered()) {
        return FALSE;
    }
    switch (element) {
        case -1:
        case HUD_RACE_POSITION:
        case HUD_RACE_POSITION_END:
        case HUD_WEAPON_DISPLAY:
        case HUD_WEAPON_QUANTITY:
        case HUD_LAP_COUNT_LABEL:
        case HUD_LAP_COUNT_CURRENT:
        case HUD_LAP_COUNT_SEPERATOR:
        case HUD_LAP_COUNT_TOTAL:
        case HUD_LAP_COUNT_FLAG:
        case HUD_BANANA_COUNT_ICON_SPIN:
        case HUD_BANANA_COUNT_NUMBER_1:
        case HUD_BANANA_COUNT_NUMBER_2:
        case HUD_BANANA_COUNT_X:
        case HUD_BANANA_COUNT_ICON_STATIC:
        case HUD_BANANA_COUNT_SPARKLE:
        case HUD_RACE_TIME_LABEL:
        case HUD_RACE_TIME_NUMBER:
        case HUD_MINIMAP_MARKER:
        case HUD_SPEEDOMETRE_ARROW:
        case HUD_SPEEDOMETRE_0:
        case HUD_SPEEDOMETRE_30:
        case HUD_SPEEDOMETRE_60:
        case HUD_SPEEDOMETRE_90:
        case HUD_SPEEDOMETRE_120:
        case HUD_SPEEDOMETRE_150:
        case HUD_SPEEDOMETRE_BG:
        case HUD_SILVER_COIN_TALLY:
        case HUD_TREASURE_METRE:
        case HUD_CHALLENGE_PORTRAIT:
        case HUD_EGG_CHALLENGE_ICON:
        case HUD_BATTLE_BANANA_ICON:
        case HUD_BATTLE_BANANA_X:
        case HUD_BATTLE_BANANA_COUNT_1:
        case HUD_BATTLE_BANANA_COUNT_2:
        case HUD_BALLOON_COUNT_ICON:
        case HUD_BALLOON_COUNT_X:
        case HUD_BALLOON_COUNT_NUMBER_1:
        case HUD_BALLOON_COUNT_NUMBER_2:
            return TRUE;
    }
    return FALSE;
}

/*
 * What the bottom screen has a HUD for (RACE_HUD_MODE_*), or -1: anything one player drives in, but not a cutscene
 * (and on the N64's screen only, the two to four player games).
 */
static s32 hud_mode(Object *player) {
    if (player == NULL || get_game_mode() != GAMEMODE_INGAME || cutscene_id() == 10 ||
        get_number_of_active_players() != 1) {
        return -1;
    }
    if (is_taj_challenge()) {
        return RACE_HUD_MODE_TAJ;
    }
    switch (level_type()) {
        case RACETYPE_DEFAULT:
        case RACETYPE_HORSESHOE_GULCH:
            return is_in_time_trial() ? RACE_HUD_MODE_TIME_TRIAL : RACE_HUD_MODE_RACE;
        case RACETYPE_BOSS:
            return RACE_HUD_MODE_BOSS;
        case RACETYPE_CHALLENGE_BANANAS:
            return RACE_HUD_MODE_BANANAS;
        case RACETYPE_CHALLENGE_BATTLE:
            return RACE_HUD_MODE_BATTLE;
        case RACETYPE_CHALLENGE_EGGS:
            return RACE_HUD_MODE_EGGS;
        case RACETYPE_HUBWORLD:
            return RACE_HUD_MODE_HUB;
    }
    return -1;
}

/* The racer's place on the map, as minimap_marker_pos works it out, in the map sprite's pixels (y down). */
static void map_position(RaceHudPoint *out, Object *obj, LevelModel *model) {
    f32 aspect, angleCos, angleSin, scaledX, scaledY, rotX, rotY, x, y;
    s32 width = model->upperXBounds - model->lowerXBounds, depth = model->upperZBounds - model->lowerZBounds;
    s32 mirrored = (get_filtered_cheats() & CHEAT_MIRRORED_TRACKS) != 0;
    s32 heading;

    if (width == 0 || depth == 0) {
        out->onMap = FALSE;
        return;
    }
    aspect = width / (f32) depth;
    angleCos = coss_f((model->minimapRotation * 0xFFFF) / 360);
    angleSin = sins_f((model->minimapRotation * 0xFFFF) / 360);
    scaledX = (60.0f * aspect * model->minimapXScale * (obj->trans.x_position - model->lowerXBounds)) / width;
    scaledY = (model->minimapYScale * -60.0f * (obj->trans.z_position - model->lowerZBounds)) / depth;
    rotX = scaledX * angleCos + scaledY * angleSin;
    rotY = scaledX * angleSin - scaledY * angleCos;
    if (mirrored) {
        // The map is flipped about its anchor: this is where the marker lands on the unflipped sprite.
        x = rotX - model->minimapOffsetXAdv2 + gMinimapDotOffsetX;
        y = rotY - model->minimapOffsetYAdv2;
    } else {
        x = rotX + model->minimapOffsetXAdv1;
        y = rotY - model->minimapOffsetYAdv1;
    }
    out->onMap = TRUE;
    out->mapX = (s16) (x * 4.0f);
    out->mapY = (s16) (y * 4.0f);
    heading = (obj->trans.rotation.y_rotation - ((model->minimapRotation * 0xFFFF) / 360)) & 0xFFFF;
    out->heading = (u16) (mirrored ? 0xFFFF - heading : heading);
}

/* The clock at each checkpoint a racer reaches for the first time. */
static void stamp(s32 index, s32 checkpoint) {
    if (checkpoint >= MAX_STAMPS) {
        checkpoint = MAX_STAMPS - 1;
    }
    while (sReached[index] < checkpoint) {
        sStamps[index][++sReached[index]] = sClock;
    }
}

static s32 gap(s32 index, s32 playerIndex, s32 checkpoints) {
    s32 both = sReached[index] < sReached[playerIndex] ? sReached[index] : sReached[playerIndex];
    s32 laps;
    if (both <= 0 || index == playerIndex) {
        return RACE_HUD_NO_GAP;
    }
    laps = sReached[index] - sReached[playerIndex];
    laps = checkpoints > 0 ? (laps < 0 ? -laps : laps) / checkpoints : 0;
    if (laps > 0) {
        return sReached[index] > sReached[playerIndex] ? RACE_HUD_LAPPED + laps : -(RACE_HUD_LAPPED + laps);
    }
    return sStamps[playerIndex][both] - sStamps[index][both];
}

/* The boss or Taj a racer is, by its vehicle (gBossVehicles in objects.c; Taj rides the carpet in his challenges). */
static s8 special_racer(Object_Racer *r) {
    switch (r->vehicleID) {
        case VEHICLE_TRICKY:
            return RACE_HUD_TRICKY;
        case VEHICLE_BLUEY:
            return RACE_HUD_BLUEY;
        case VEHICLE_SMOKEY:
        case VEHICLE_PTERODACTYL:
            return RACE_HUD_SMOKEY;
        case VEHICLE_BUBBLER:
            return RACE_HUD_BUBBLER;
        case VEHICLE_WIZPIG:
        case VEHICLE_ROCKET:
            return RACE_HUD_WIZPIG;
        case VEHICLE_CARPET:
            return is_taj_challenge() ? RACE_HUD_TAJ : RACE_HUD_NOT_SPECIAL;
    }
    return RACE_HUD_NOT_SPECIAL;
}

/*
 * Time trial: the best run's splits, kept per track and vehicle in the data directory (ttbest_LEVEL_VEHICLE.bin) so
 * the bottom screen can show how a run compares with it as it goes, checkpoint by checkpoint, as the ghost does on
 * the track. A run that beats it replaces it at the finish.
 */
#define BEST_MAGIC 0x54544231 // "TTB1"
typedef struct BestRun {
    s32 magic;
    s32 total;
    s32 lapCount;
    s32 laps[RACE_HUD_LAPS];
    s32 stampCount;
    u16 stamps[MAX_STAMPS];
} BestRun;

static BestRun sBest;
static s32 sBestLevel = -1, sBestVehicle = -1, sBestSaved;

static void best_path(char *out, s32 size, s32 level, s32 vehicle) {
    plat_snprintf(out, size, "%s/ttbest_%d_%d.bin", plat_data_dir(), (int) level, (int) vehicle);
}

static void best_load(s32 level, s32 vehicle) {
    char path[256];
    unsigned int size = 0;
    void *data;
    if (level == sBestLevel && vehicle == sBestVehicle) {
        return;
    }
    sBestLevel = level;
    sBestVehicle = vehicle;
    sBest.magic = 0;
    best_path(path, sizeof(path), level, vehicle);
    data = plat_load_file(path, &size);
    if (data != NULL && size == sizeof(BestRun) && ((BestRun *) data)->magic == BEST_MAGIC) {
        sBest = *(BestRun *) data;
    }
    free(data);
}

/*
 * The tracks menu's companion shows each track's best total as the cursor moves. Opening a file on the SD card for
 * every move held the game thread for 150 ms and more (bug report #217: the data directory keeps every bug report
 * snapshot, and looking up a name that isn't there reads all of it), so the totals are read once, the first time a
 * menu shows a companion: one pass over the directory, opening only the ttbest files it has.
 */
#define BEST_LEVELS 128
#define BEST_VEHICLES 3
static s32 sBestTotals[BEST_LEVELS][BEST_VEHICLES]; // 0: none.
static s32 sBestTotalsRead;

/* Reads a number at *text and moves past it; -1 if there is none. */
static s32 best_number(const char **text) {
    s32 value = -1;
    while (**text >= '0' && **text <= '9' && value < 1000) {
        value = (value < 0 ? 0 : value * 10) + (**text - '0');
        (*text)++;
    }
    return value;
}

static void best_totals_read(void) {
    static char sNames[BEST_LEVELS * BEST_VEHICLES][24];
    char path[256];
    unsigned long long start = plat_time_ns();
    s32 count, found = 0, i;
    sBestTotalsRead = TRUE;
    count = plat_list_dir_prefix(plat_data_dir(), "ttbest_", sNames[0], sizeof(sNames[0]), ARRAY_COUNT(sNames));
    for (i = 0; i < count; i++) {
        // ttbest_LEVEL_VEHICLE.bin
        const char *text = sNames[i] + 7;
        s32 level = best_number(&text), vehicle = -1;
        unsigned int size = 0;
        void *data;
        if (*text == '_') {
            text++;
            vehicle = best_number(&text);
        }
        if (level < 0 || level >= BEST_LEVELS || vehicle < 0 || vehicle >= BEST_VEHICLES ||
            text[0] != '.' || text[1] != 'b' || text[2] != 'i' || text[3] != 'n' || text[4] != '\0') {
            continue;
        }
        best_path(path, sizeof(path), level, vehicle);
        data = plat_load_file(path, &size);
        if (data != NULL && size == sizeof(BestRun) && ((BestRun *) data)->magic == BEST_MAGIC) {
            sBestTotals[level][vehicle] = ((BestRun *) data)->total;
            found++;
        }
        free(data);
    }
    plat_log("time trial bests: %d read in %d ms", (int) found, (int) ((plat_time_ns() - start) / 1000000ULL));
}

/* At the finish: this run becomes the best if it is (the stamps are the player's, sStamps[playerIndex]). */
static void best_finish(s32 playerIndex, const RaceHud *hud) {
    char path[256];
    s32 i;
    if (sBestSaved || hud->raceTime <= 0 || (sBest.magic == BEST_MAGIC && sBest.total <= hud->raceTime)) {
        sBestSaved = TRUE;
        return;
    }
    sBestSaved = TRUE;
    sBest.magic = BEST_MAGIC;
    sBest.total = hud->raceTime;
    sBest.lapCount = hud->lapsTimed;
    for (i = 0; i < RACE_HUD_LAPS; i++) {
        sBest.laps[i] = i < hud->lapsTimed ? hud->lapTimes[i] : 0;
    }
    sBest.stampCount = sReached[playerIndex] + 1;
    for (i = 0; i < MAX_STAMPS; i++) {
        s32 t = i < sBest.stampCount ? sStamps[playerIndex][i] : 0;
        sBest.stamps[i] = (u16) (t > 0xFFFF ? 0xFFFF : t);
    }
    best_path(path, sizeof(path), sBestLevel, sBestVehicle);
    plat_save_file(path, &sBest, sizeof(sBest));
    if (sBestLevel >= 0 && sBestLevel < BEST_LEVELS && sBestVehicle >= 0 && sBestVehicle < BEST_VEHICLES) {
        sBestTotals[sBestLevel][sBestVehicle] = sBest.total;
    }
}

/* The bosses' rematches (the second race in each world). */
static s32 boss_round(void) {
    switch (level_id()) {
        case ASSET_LEVEL_TRICKYTOPS2:
        case ASSET_LEVEL_BLUEY2:
        case ASSET_LEVEL_BUBBLER2:
        case ASSET_LEVEL_SMOKEY2:
        case ASSET_LEVEL_WIZPIG2:
            return 2;
    }
    return 1;
}

/* A challenge's score: bananas home, or eggs in the nest; in battle, bananas left. */
static s32 challenge_score(s32 mode, Object_Racer *r) {
    return mode == RACE_HUD_MODE_BATTLE ? r->bananas : r->lap;
}

/* The hub's progress: balloons, each world's balloons, trophy, key and boss, the amulets, the doors and Taj. */
static void hub_progress(RaceHud *hud, Settings *settings, LevelModel *model) {
    Object **objects;
    s32 i, start, count;
    hud->world = settings->worldId;
    hud->balloons = settings->balloonsPtr[0];
    for (i = 0; i < RACE_HUD_WORLDS; i++) {
        hud->worldBalloons[i] = settings->balloonsPtr[i + 1];
        hud->worldTrophy[i] = (settings->trophies >> (i * 2)) & 3;
    }
    hud->worldKeys = (settings->keys >> 1) & 0x1F;
    hud->worldBosses = (settings->bosses >> 1) & 0x1F;
    hud->ttAmulet = settings->ttAmulet;
    hud->wizpigAmulet = settings->wizpigAmulet;
    if (model == NULL) {
        return;
    }
    objects = objGetObjList(&start, &count);
    for (i = start; i < count; i++) {
        Object *obj = objects[i];
        if (obj == NULL || (obj->trans.flags & OBJ_FLAGS_PARTICLE)) {
            continue;
        }
        if (obj->behaviorId == BHV_DOOR && hud->doorCount < RACE_HUD_DOORS) {
            map_position(&hud->doors[hud->doorCount++], obj, model);
        } else if (obj->behaviorId == BHV_PARK_WARDEN && !hud->taj.onMap) {
            map_position(&hud->taj, obj, model);
        }
    }
}

/* Time trial: the course records, T.T., the ghosts, and how this run compares with the best one. */
static void time_trial_progress(RaceHud *hud, Object_Racer *me, LevelModel *model) {
    Settings *settings = get_settings();
    s32 vehicle = me->vehicleID, i, at;
    Object *ghost;
    hud->record = hud->recordLap = hud->staffTime = -1;
    hud->bestGap = RACE_HUD_NO_GAP;
    if (vehicle >= 0 && vehicle < 3) {
        u16 course = settings->courseTimesPtr[vehicle][settings->courseId];
        u16 lap = settings->flapTimesPtr[vehicle][settings->courseId];
        // 10800 (three minutes) and over is the empty record.
        hud->record = course > 0 && course < 10800 ? course : -1;
        hud->recordLap = lap > 0 && lap < 10800 ? lap : -1;
    }
    if (timetrial_ghost_staff() != NULL) {
        hud->staffTime = gTTGhostTimeToBeat;
    }
    best_load(level_id(), vehicle);
    if (sBest.magic == BEST_MAGIC) {
        hud->bestTotal = sBest.total;
        hud->bestLapCount = sBest.lapCount;
        for (i = 0; i < RACE_HUD_LAPS; i++) {
            hud->bestLaps[i] = sBest.laps[i];
        }
        at = me->racerIndex >= 0 && me->racerIndex < RACE_HUD_RACERS ? sReached[me->racerIndex] : 0;
        if (at > 0 && at < sBest.stampCount && at < MAX_STAMPS) {
            hud->bestGap = sStamps[me->racerIndex][at] - sBest.stamps[at];
        }
    }
    if (model != NULL) {
        ghost = timetrial_player_ghost();
        if (ghost != NULL && !(ghost->trans.flags & OBJ_FLAGS_INVISIBLE)) {
            map_position(&hud->ghost, ghost, model);
        }
        ghost = timetrial_ghost_staff();
        if (ghost != NULL && !(ghost->trans.flags & OBJ_FLAGS_INVISIBLE)) {
            map_position(&hud->staffGhost, ghost, model);
        }
    }
}

/* Not racing: what the menu has highlighted, for the bottom screen (RACE_HUD_MODE_MENU_*). FALSE: nothing. */
static s32 menu_companion(RaceHud *hud) {
    static const RaceHud sEmpty;
    Settings *file = NULL;
    char *name = NULL;
    s32 level = -1, vehicle = 0;
    if (get_game_mode() != GAMEMODE_MENU) {
        return FALSE;
    }
    if (!sBestTotalsRead) {
        best_totals_read();
    }
    switch (port_menu_focus(&level, &vehicle, &file, &name)) {
        case PORT_MENU_FOCUS_FILE:
            *hud = sEmpty;
            hud->mode = RACE_HUD_MODE_MENU_SAVE;
            hud->mapSprite = -1;
            hub_progress(hud, file, NULL);
            hud->world = -1;
            hud->adventureTwo = (file->cutsceneFlags & CUTSCENE_ADVENTURE_TWO) != 0;
            plat_snprintf(hud->name, sizeof(hud->name), "%.3s", name);
            return TRUE;
        case PORT_MENU_FOCUS_TRACK: {
            Settings *settings = get_settings();
            char *trackName = level_name(level);
            *hud = sEmpty;
            hud->mode = RACE_HUD_MODE_MENU_TRACK;
            hud->mapSprite = -1;
            hud->level = level;
            hud->vehicle = vehicle;
            hud->record = hud->recordLap = hud->bestTotal = -1;
            if (vehicle >= 0 && vehicle < BEST_VEHICLES && level >= 0) {
                u16 course = settings->courseTimesPtr[vehicle][level];
                u16 lap = settings->flapTimesPtr[vehicle][level];
                hud->record = course > 0 && course < 10800 ? course : -1;
                hud->recordLap = lap > 0 && lap < 10800 ? lap : -1;
                if (level < BEST_LEVELS && sBestTotals[level][vehicle] > 0) {
                    hud->bestTotal = sBestTotals[level][vehicle];
                }
            }
            plat_snprintf(hud->name, sizeof(hud->name), "%s", trackName != NULL ? trackName : "");
            return TRUE;
        }
    }
    return FALSE;
}

/* Once a game frame (src/thread3_main.c), with the number of retraces it took. */
void port_race_hud_frame(s32 updateRate) {
    static const RaceHud sEmpty;
    static RaceHud hud;
    Object *player = get_racer_object_by_port(PLAYER_ONE);
    Object **racers;
    Object_Racer *me;
    LevelModel *model;
    LevelHeader *header;
    s32 count, i, j, checkpoints, mode = hud_mode(player);
    s32 order[RACE_HUD_RACERS];

    if (mode < 0 && port_race_hud_setting() != RACE_HUD_TOP && menu_companion(&hud)) {
        sActive = FALSE;
        sMenuShown = TRUE;
        plat_set_race_hud(&hud);
        return;
    }
    if (mode >= 0) {
        racers = get_racer_objects(&count);
        if (count > RACE_HUD_RACERS) {
            count = RACE_HUD_RACERS;
        }
        // The clock and the checkpoints are kept whichever screen the HUD is on, so that switching it to the bottom
        // in the middle of a race doesn't start the gaps (or a time trial's splits) over from there.
        if (!sTracking || get_race_countdown() > 0 || mode != sTrackedMode) {
            // A new race (or the start of this one): the clock and the gaps start over.
            sClock = 0;
            sBestSaved = FALSE;
            for (i = 0; i < RACE_HUD_RACERS; i++) {
                sReached[i] = 0;
            }
        } else if (!is_game_paused()) {
            sClock += updateRate;
        }
        sTracking = TRUE;
        sTrackedMode = mode;
        for (i = 0; i < count; i++) {
            Object_Racer *r = racers[i]->racer;
            if (r->racerIndex >= 0 && r->racerIndex < RACE_HUD_RACERS) {
                stamp(r->racerIndex, r->courseCheckpoint);
            }
        }
    } else {
        sTracking = FALSE;
    }
    if (port_race_hud_setting() == RACE_HUD_TOP || mode < 0) {
        if (sActive || sMenuShown) {
            sActive = FALSE;
            sMenuShown = FALSE;
            plat_set_race_hud(NULL);
        }
        return;
    }
    sMenuShown = FALSE;
    me = player->racer;
    header = level_header();
    model = get_current_level_model();
    checkpoints = get_checkpoint_count();
    sActive = TRUE;

    hud = sEmpty;
    hud.mode = mode;
    hud.mapSprite = model != NULL && model == sMapModel ? sMapSprite : -1;
    hud.mapMirrored = (get_filtered_cheats() & CHEAT_MIRRORED_TRACKS) != 0;
    hud.silverCoins = -1;
    if (model != NULL) {
        hud.mapColour[0] = (model->minimapColor >> 16) & 0xFF;
        hud.mapColour[1] = (model->minimapColor >> 8) & 0xFF;
        hud.mapColour[2] = model->minimapColor & 0xFF;
    }
    // The racers in race order (racePosition from 1; a finished racer keeps its finishing place); in a challenge, by
    // score (the knocked out last).
    for (i = 0; i < count; i++) {
        order[i] = i;
    }
    for (i = 1; i < count; i++) {
        for (j = i; j > 0; j--) {
            Object_Racer *a = racers[order[j - 1]]->racer, *b = racers[order[j]]->racer;
            s32 swap;
            if (mode == RACE_HUD_MODE_BANANAS || mode == RACE_HUD_MODE_BATTLE || mode == RACE_HUD_MODE_EGGS) {
                s32 outA = mode == RACE_HUD_MODE_BATTLE && a->raceFinished && a->bananas <= 0;
                s32 outB = mode == RACE_HUD_MODE_BATTLE && b->raceFinished && b->bananas <= 0;
                swap = outA != outB ? outA : challenge_score(mode, b) > challenge_score(mode, a);
            } else {
                swap = b->racePosition < a->racePosition;
            }
            if (!swap) {
                break;
            }
            swap = order[j];
            order[j] = order[j - 1];
            order[j - 1] = swap;
        }
    }
    hud.racerCount = 0;
    for (j = 0; j < count; j++) {
        Object_Racer *r = racers[order[j]]->racer;
        RaceHudRacer *out = &hud.racers[hud.racerCount++];
        out->character = r->characterId;
        out->special = special_racer(r);
        if (out->special == RACE_HUD_NOT_SPECIAL && r->vehicleID >= VEHICLE_BOSSES) {
            // A boss vehicle with no portrait of its own: its characterId is a stand-in, not its face.
            out->character = -1;
        }
        out->player = racers[order[j]] == player;
        out->gap = (r->racerIndex >= 0 && r->racerIndex < RACE_HUD_RACERS && me->racerIndex >= 0 &&
                    me->racerIndex < RACE_HUD_RACERS)
                       ? gap(r->racerIndex, me->racerIndex, checkpoints)
                       : RACE_HUD_NO_GAP;
        out->score = (s8) challenge_score(mode, r);
        if (mode == RACE_HUD_MODE_BANANAS) {
            out->carrying = r->bananas > 0;
        } else if (mode == RACE_HUD_MODE_EGGS) {
            out->carrying = r->held_obj != NULL;
        }
        out->out = mode == RACE_HUD_MODE_BATTLE && r->raceFinished && r->bananas <= 0;
        if (mode == RACE_HUD_MODE_BATTLE && !out->out) {
            hud.racersLeft++;
        }
        if (model != NULL) {
            map_position(&out->at, racers[order[j]], model);
        }
    }
    hud.position = me->racePosition;
    // The race types whose top screen HUD counts laps (game_ui.c): the races, Horseshoe Gulch's and the bosses'.
    hud.laps = (level_type() == RACETYPE_DEFAULT || level_type() == RACETYPE_HORSESHOE_GULCH ||
                level_type() == RACETYPE_BOSS) &&
                       mode != RACE_HUD_MODE_HUB
                   ? header->laps
                   : 0;
    hud.lap = me->countLap + 1;
    if (hud.laps > 0 && hud.lap > hud.laps) {
        hud.lap = hud.laps;
    }
    for (i = 0; i <= me->countLap && i < RACE_HUD_LAPS && (hud.laps == 0 || i < hud.laps); i++) {
        hud.lapTimes[i] = me->lap_times[i];
        hud.raceTime += me->lap_times[i];
        hud.lapsTimed = i + 1;
    }
    hud.item = me->balloon_quantity > 0 ? me->balloon_type : -1;
    hud.itemLevel = me->balloon_level;
    hud.itemCount = me->balloon_quantity;
    hud.bananas = me->bananas;
    hud.countdown = get_race_countdown() > 0;
    hud.finished = me->raceFinished;
    hud.vehicle = me->vehicleID;
    switch (mode) {
        case RACE_HUD_MODE_RACE:
            if (check_if_silver_coin_race()) {
                hud.silverCoins = me->silverCoinCount;
            }
            break;
        case RACE_HUD_MODE_BOSS:
            hud.round = boss_round();
            break;
        case RACE_HUD_MODE_BANANAS:
            hud.goal = 10;
            break;
        case RACE_HUD_MODE_EGGS:
            hud.goal = 3;
            break;
        case RACE_HUD_MODE_TIME_TRIAL:
            time_trial_progress(&hud, me, model);
            if (hud.finished && me->racerIndex >= 0 && me->racerIndex < RACE_HUD_RACERS) {
                best_finish(me->racerIndex, &hud);
            }
            break;
        case RACE_HUD_MODE_HUB:
            hub_progress(&hud, get_settings(), model);
            break;
    }
    plat_set_race_hud(&hud);
}

/* The Race HUD page of the Settings menu (and SCREEN OPTIONS in the game's menus): where the HUD goes, and how the bottom screen looks. */
int race_hud_option_count(void) {
    return 2;
}

const char *race_hud_option_label(int index) {
    return index == 1 ? "Bottom screen look" : "Race HUD on";
}

static const char *const sLookNames[4] = { "Classic", "Glass", "Broadcast", "Dock" };

s32 port_bottom_style(void) {
    const char *env = plat_getenv("DKR_BOTTOM_STYLE");
    s32 value = env != NULL ? atoi(env) : settings_get("bottomstyle", 0);
    return (value < 0 || value > 3) ? 0 : value;
}

void race_hud_option_value(int index, char *out, int size) {
    static const char *const sNames[3] = { "Top", "Bottom", "Both" };
    if (index == 1) {
        plat_snprintf(out, size, "%s", sLookNames[port_bottom_style()]);
        return;
    }
    plat_snprintf(out, size, "%s", sNames[port_race_hud_setting()]);
}

void race_hud_option_change(int index, int step) {
    // Bottom, Both, Top: the order the row steps through.
    static const s32 sOrder[3] = { RACE_HUD_BOTTOM, RACE_HUD_BOTH, RACE_HUD_TOP };
    s32 at = 0;
    if (index == 1) {
        s32 look = (port_bottom_style() + (step < 0 ? 3 : 1)) % 4;
        settings_set("bottomstyle", look);
        plat_set_bottom_style(look);
        return;
    }
    while (at < 2 && sOrder[at] != port_race_hud_setting()) {
        at++;
    }
    port_set_race_hud_setting(sOrder[(at + (step < 0 ? 2 : 1)) % 3]);
}

const char *race_hud_option_hint(int index) {
    if (index == 1) {
        static const char *const sHints[4] = { "The game's own boxes", "Soft glass cards, round buttons",
                                               "Race TV slabs in red and yellow", "Few boxes, icon bar below" };
        return sHints[port_bottom_style()];
    }
    switch (port_race_hud_setting()) {
        case RACE_HUD_TOP:
            return "Lap, item, map on the top screen";
        case RACE_HUD_BOTH:
            return "On both screens";
        default:
            return "Standings, item and map below";
    }
}
