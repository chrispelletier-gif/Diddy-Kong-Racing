/*
 * Test helpers for automated play-throughs. Everything here is off unless its setting is present (environment
 * variables on the host, sdmc:/3ds/dkr/env.txt on the 3DS).
 *
 *   DKR_WARP=all | id,id,...   The title screen's attract demos load these levels instead, one after another, with
 *                              the AI driving. "all" covers every level. Level ids are the ASSET_LEVEL_* order.
 *   DKR_WARP_SECONDS=n         How long each warped level is shown (default 20).
 *   DKR_TEST_SAVE=1            Selecting a save file gives it complete progress (see port_apply_test_save).
 */
#include <ultra64.h>
#include "types.h"
#include "structs.h"
#include "asset_enums.h"
#include "game.h"
#include "thread3_main.h"
#include "port.h"
#include "plat/plat.h"

#define MAX_WARP_LEVELS 128

static s16 sWarpLevels[MAX_WARP_LEVELS];
static s32 sNumWarpLevels = -1;
static s32 sWarpNext;

static void parse_warp_list(void) {
    const char *list = plat_getenv("DKR_WARP");
    s32 i, numLevels, numWorlds;

    sNumWarpLevels = 0;
    if (list == NULL) {
        return;
    }
    if (list[0] == 'a') {
        level_count(&numLevels, &numWorlds);
        for (i = 0; i < numLevels && i < MAX_WARP_LEVELS; i++) {
            sWarpLevels[sNumWarpLevels++] = i;
        }
    } else {
        while (*list && sNumWarpLevels < MAX_WARP_LEVELS) {
            sWarpLevels[sNumWarpLevels++] = port_atoi(list);
            while (*list && *list != ',') {
                list++;
            }
            if (*list == ',') {
                list++;
            }
        }
    }
    port_log("warp: %d levels", (int) sNumWarpLevels);
}

/* Nonzero while DKR_WARP replaces the title demos (the title overlay is hidden then). */
s32 port_warp_active(void) {
    if (sNumWarpLevels < 0) {
        parse_warp_list();
    }
    return sNumWarpLevels > 0;
}

/*
 * Called whenever the title screen picks the level for its next demo. Returns the level to load instead, and sets
 * *timer (in 60ths of a second) to how long to show it, or leaves both alone when no warp is active.
 */
s32 port_warp_next(s32 levelId, s32 *cutsceneId, s16 *timer) {
    const char *seconds;
    s32 level;

    if (sNumWarpLevels < 0) {
        parse_warp_list();
    }
    if (sNumWarpLevels == 0) {
        return levelId;
    }
    if (sWarpNext >= sNumWarpLevels) {
        port_log("warp: done");
        sWarpNext = 0;
    }
    level = sWarpLevels[sWarpNext++];
    seconds = plat_getenv("DKR_WARP_SECONDS");
    *timer = ((seconds != NULL) ? port_atoi(seconds) : 20) * 60;
    *cutsceneId = 0;
    port_log("warp: level %d at frame %d", (int) level, (int) port_gfx_frame_count());
    return level;
}

/*
 * Gives a save file everything: all balloons, keys, trophies, amulet pieces and Taj challenges, every boss beaten,
 * every course completed (including silver coins) and every hub door open, and the story cutscenes marked as seen.
 * Adventure Two is left off, so the tracks are not mirrored.
 */
s32 port_apply_test_save(void *settingsPtr) {
    Settings *settings = (Settings *) settingsPtr;
    s32 numLevels, numWorlds, i;

    if (plat_getenv("DKR_TEST_SAVE") == NULL) {
        return FALSE;
    }
    level_count(&numLevels, &numWorlds);
    settings->balloonsPtr[0] = 47;
    for (i = 1; i < numWorlds; i++) {
        settings->balloonsPtr[i] = 8;
    }
    for (i = 0; i < numLevels; i++) {
        settings->courseFlagsPtr[i] |= 0xFFFF0007;
    }
    settings->keys = 0x1E;
    settings->bosses = 0x07BF;
    settings->trophies = 0x03FF;
    settings->cutsceneFlags |= 0x3FFFB; // Everything but 0x4 (Adventure Two).
    settings->tajFlags = 0x3F;
    settings->ttAmulet = 4;
    settings->wizpigAmulet = 4;
    settings->newGame = FALSE;
    port_log("test save: applied (%d levels, %d worlds)", (int) numLevels, (int) numWorlds);
    return TRUE;
}

const char *volatile gPortGameStage = "starting";
extern f32 gObjectUpdateRateF; // objects.c

/* port.h: for the watchdog's hang message. Read from another thread, so it may be a frame out of date. */
void port_describe_game_state(char *buffer, s32 size) {
    s32 mode = get_game_mode();
    plat_snprintf(buffer, size, "game thread %s; %s, level %d%s, logic rate %d", gPortGameStage,
                  mode == GAMEMODE_INTRO    ? "intro"
                  : mode == GAMEMODE_INGAME ? "in game"
                  : mode == GAMEMODE_MENU   ? "menu"
                                            : "other mode",
                  (int) level_id(), is_game_paused() ? ", paused" : "", (int) gObjectUpdateRateF);
}
