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
 * finish), 2 both screens, 0 the top screen only, as on the N64 (the default on PC, which has no bottom screen). src/game_ui.c asks port_race_hud_hides() which of
 * its pieces to leave out.
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
#include <stdlib.h>

/* The bottom screen's side (bottom_screen.c on the 3DS; the host build keeps the last one for its preview). */
void plat_set_race_hud(const RaceHud *hud);

extern s32 gMinimapDotOffsetX;

/* How many checkpoints a racer's gaps can be timed at: (laps + 3) * checkpoints is the most a race counts. */
#define MAX_STAMPS 768

static volatile s32 sSetting = -1; // RACE_HUD_*, -1 until loaded.
static s32 sActive;                // The bottom screen is showing this race's HUD.
static s32 sMapSprite = -1;        // The current track's map, as minimap_init loaded it.
static s32 sClock;                 // 60ths of a second since the start.
static s16 sReached[RACE_HUD_RACERS];
static s32 sStamps[RACE_HUD_RACERS][MAX_STAMPS];

enum { RACE_HUD_TOP, RACE_HUD_BOTTOM, RACE_HUD_BOTH };

s32 port_race_hud_setting(void) {
    if (sSetting < 0) {
        const char *env = plat_getenv("DKR_RACE_HUD");
#ifdef __3DS__
        s32 fallback = RACE_HUD_BOTTOM;
#else
        // No bottom screen on PC: the N64's HUD, unless its pictures are wanted (DKR_BOTTOM_RACE).
        s32 fallback = plat_getenv("DKR_BOTTOM_RACE") != NULL ? RACE_HUD_BOTTOM : RACE_HUD_TOP;
#endif
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
void port_race_hud_map(s32 spriteIndex) {
    sMapSprite = spriteIndex;
}

/* Whether game_ui.c leaves this HUD element (HUD_*, or -1 for the track map itself) off the top screen. */
s32 port_race_hud_hides(s32 element) {
    if (!sActive || port_race_hud_setting() != RACE_HUD_BOTTOM) {
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
            return TRUE;
    }
    return FALSE;
}

/* The races the bottom screen has a HUD for: one player, a race with laps (or a boss race), not a time trial. */
static s32 hud_race(Object *player) {
    s32 type;
    if (player == NULL || get_game_mode() != GAMEMODE_INGAME || cutscene_id() == 10 || is_in_time_trial() ||
        is_taj_challenge() || get_number_of_active_players() != 1) {
        return FALSE;
    }
    type = level_type();
    return type == RACETYPE_DEFAULT || type == RACETYPE_HORSESHOE_GULCH || type == RACETYPE_BOSS;
}

/* The racer's place on the map, as minimap_marker_pos works it out, in the map sprite's pixels (y down). */
static void map_position(RaceHudRacer *out, Object *obj, LevelModel *model) {
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

/* Once a game frame (src/thread3_main.c), with the number of retraces it took. */
void port_race_hud_frame(s32 updateRate) {
    static const RaceHud sEmpty;
    static RaceHud hud;
    Object *player = get_racer_object_by_port(PLAYER_ONE);
    Object **racers;
    Object_Racer *me;
    LevelModel *model;
    LevelHeader *header;
    s32 count, i, j, checkpoints;

    if (port_race_hud_setting() == RACE_HUD_TOP || !hud_race(player)) {
        if (sActive) {
            sActive = FALSE;
            plat_set_race_hud(NULL);
        }
        return;
    }
    me = player->racer;
    header = level_header();
    racers = get_racer_objects(&count);
    model = get_current_level_model();
    checkpoints = get_checkpoint_count();
    if (count > RACE_HUD_RACERS) {
        count = RACE_HUD_RACERS;
    }
    if (!sActive || get_race_countdown() > 0) {
        // A new race (or the start of this one): the clock and the gaps start over.
        sClock = 0;
        for (i = 0; i < RACE_HUD_RACERS; i++) {
            sReached[i] = 0;
        }
    } else if (!is_game_paused()) {
        sClock += updateRate;
    }
    sActive = TRUE;

    hud = sEmpty;
    hud.mapSprite = sMapSprite;
    hud.mapMirrored = (get_filtered_cheats() & CHEAT_MIRRORED_TRACKS) != 0;
    if (model != NULL) {
        hud.mapColour[0] = (model->minimapColor >> 16) & 0xFF;
        hud.mapColour[1] = (model->minimapColor >> 8) & 0xFF;
        hud.mapColour[2] = model->minimapColor & 0xFF;
    }
    for (i = 0; i < count; i++) {
        Object_Racer *r = racers[i]->racer;
        if (r->racerIndex >= 0 && r->racerIndex < RACE_HUD_RACERS) {
            stamp(r->racerIndex, r->courseCheckpoint);
        }
    }
    // The racers in race order (racePosition from 1; a finished racer keeps its finishing place).
    hud.racerCount = 0;
    for (j = 1; j <= RACE_HUD_RACERS && hud.racerCount < count; j++) {
        for (i = 0; i < count; i++) {
            Object_Racer *r = racers[i]->racer;
            RaceHudRacer *out;
            if (r->racePosition != j) {
                continue;
            }
            out = &hud.racers[hud.racerCount++];
            out->character = r->characterId;
            out->player = racers[i] == player;
            out->gap = (r->racerIndex >= 0 && r->racerIndex < RACE_HUD_RACERS)
                           ? gap(r->racerIndex, me->racerIndex, checkpoints)
                           : RACE_HUD_NO_GAP;
            if (model != NULL) {
                map_position(out, racers[i], model);
            }
        }
    }
    hud.position = me->racePosition;
    hud.laps = level_type() == RACETYPE_DEFAULT ? header->laps : 0;
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
    plat_set_race_hud(&hud);
}

/* The Race HUD row of the Settings menu (and VIDEO OPTIONS in the game's menus). */
int race_hud_option_count(void) {
    return 1;
}

const char *race_hud_option_label(int index) {
    (void) index;
    return "Race HUD on";
}

void race_hud_option_value(int index, char *out, int size) {
    static const char *const sNames[3] = { "Top", "Bottom", "Both" };
    (void) index;
    plat_snprintf(out, size, "%s", sNames[port_race_hud_setting()]);
}

void race_hud_option_change(int index, int step) {
    // Bottom, Both, Top: the order the row steps through.
    static const s32 sOrder[3] = { RACE_HUD_BOTTOM, RACE_HUD_BOTH, RACE_HUD_TOP };
    s32 at = 0;
    (void) index;
    while (at < 2 && sOrder[at] != port_race_hud_setting()) {
        at++;
    }
    port_set_race_hud_setting(sOrder[(at + (step < 0 ? 2 : 1)) % 3]);
}

const char *race_hud_option_hint(int index) {
    (void) index;
    switch (port_race_hud_setting()) {
        case RACE_HUD_TOP:
            return "Lap, item, map on the top screen";
        case RACE_HUD_BOTH:
            return "On both screens";
        default:
            return "Standings, item and map below";
    }
}
