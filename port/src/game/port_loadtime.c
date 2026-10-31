/*
 * Level load timing. load_level_game / load_level_menu call port_load_begin and port_load_end around the load, and
 * the steps inside it call port_load_step with their name when they finish, so the log gets one line per load:
 *
 *   level load 12 (game): 812 ms: rest 3, track 420, objects 210, racers 95, shadows 40, hud 4; first frame +180 ms
 *
 * "first frame" is how long after the load the renderer finished the first frame drawn after it (texture decoding at
 * load, P8, happens there). The tracks menu's loads of the track under the cursor run on the game's background thread
 * and say "background". When a race was picked in the tracks menu, the line also says how long the player has waited
 * since pressing A on the track and on the last option. A game frame that takes more than LONG_FRAME_MS is logged too.
 * Always on: a load is rare and the marks cost a clock read each.
 */
#include <ultra64.h>
#include "types.h"
#include "port.h"
#include "plat/plat.h"

#define MAX_STEPS 12
#define LONG_FRAME_MS 150

static const char *sStepName[MAX_STEPS];
static unsigned long long sStepNs[MAX_STEPS];
static s32 sStepCount;
static unsigned long long sLoadStartNs;
static unsigned long long sLastMarkNs;
static s32 sActive;
static char sLine[320];
static volatile s32 sWaitFirstFrame;
static unsigned long long sLoadEndNs;
static s32 sLoadWasGame;
static unsigned long long sPickNs[2]; // Tracks menu: A on the track, A on the last option (0 = none).
static unsigned long long sLastGameFrameNs;
static unsigned long long sBgQueuedNs; // When the tracks menu asked for a background load.
static s32 sBgStartDelayMs;           // How long the background load then took to start, or -1.

/* The tracks menu asked the background thread to load a track (bgload_start). */
void port_load_bg_queued(void) {
    sBgQueuedNs = plat_time_ns();
}

void port_load_begin(void) {
    sBgStartDelayMs = -1;
    if (sBgQueuedNs != 0) {
        sBgStartDelayMs = (s32) ((plat_time_ns() - sBgQueuedNs) / 1000000ULL);
        sBgQueuedNs = 0;
    }
    sStepCount = 0;
    sLoadStartNs = sLastMarkNs = plat_time_ns();
    sActive = TRUE;
}

static s32 same_name(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

void port_load_step(const char *name) {
    unsigned long long now;
    s32 i;
    if (!sActive) {
        return;
    }
    now = plat_time_ns();
    for (i = 0; i < sStepCount; i++) {
        if (same_name(sStepName[i], name)) {
            break;
        }
    }
    if (i == sStepCount) {
        if (sStepCount == MAX_STEPS) {
            sLastMarkNs = now;
            return;
        }
        sStepName[sStepCount] = name;
        sStepNs[sStepCount++] = 0;
    }
    sStepNs[i] += now - sLastMarkNs;
    sLastMarkNs = now;
}

void port_load_end(s32 levelId, const char *kind) {
    unsigned long long now;
    s32 len;
    s32 i;
    if (!sActive) {
        return;
    }
    port_load_step("rest");
    sActive = FALSE;
    now = plat_time_ns();
    len = plat_snprintf(sLine, sizeof(sLine), "level load %d (%s): %d ms:", (int) levelId, kind,
                        (int) ((now - sLoadStartNs) / 1000000ULL));
    for (i = 0; i < sStepCount && len < (s32) sizeof(sLine) - 1; i++) {
        len += plat_snprintf(sLine + len, sizeof(sLine) - len, "%s %s %d", i ? "," : "", sStepName[i],
                             (int) ((sStepNs[i] + 500000ULL) / 1000000ULL));
    }
    if (sBgStartDelayMs >= 0 && kind[0] == 'b' && len < (s32) sizeof(sLine) - 1) {
        len += plat_snprintf(sLine + len, sizeof(sLine) - len, " (started %d ms after it was asked for)",
                             (int) sBgStartDelayMs);
    }
    sLoadEndNs = now;
    sLoadWasGame = kind[0] == 'g';
    sWaitFirstFrame = TRUE;
}

/* Tracks menu: the player pressed A on a track (step 0) or on the last option before the race loads (step 1). */
void port_load_pick(s32 step) {
    sPickNs[step] = plat_time_ns();
    if (step == 0) {
        sPickNs[1] = 0;
    }
}

/*
 * Tracks menu, every frame: whether it is holding a step (zooming in on the track, or starting the race) until the
 * background load of the track under the cursor is done. Logs how long it held once it lets go.
 */
void port_load_menu_held(s32 held) {
    static unsigned long long sHeldSinceNs;
    unsigned long long now;
    if (held && sHeldSinceNs == 0) {
        sHeldSinceNs = plat_time_ns();
    } else if (!held && sHeldSinceNs != 0) {
        now = plat_time_ns();
        if (now - sHeldSinceNs > 30000000ULL) {
            plat_log("tracks menu waited %d ms for the background load", (int) ((now - sHeldSinceNs) / 1000000ULL));
        }
        sHeldSinceNs = 0;
    }
}

/* Game thread, at the top of every frame: logs a frame that took much longer than it should. */
void port_load_game_frame(s32 gameMode, s32 levelId) {
    unsigned long long now = plat_time_ns();
    if (sLastGameFrameNs != 0 && now - sLastGameFrameNs > LONG_FRAME_MS * 1000000ULL) {
        plat_log("long game frame: %d ms (mode %d, level %d)", (int) ((now - sLastGameFrameNs) / 1000000ULL),
                 (int) gameMode, (int) levelId);
    }
    sLastGameFrameNs = now;
}

/* Renderer: a frame has been handed to the GPU. The first one after a load finishes the load's log line. */
void port_gfx_log_cache(void);

void port_load_frame_done(void) {
    unsigned long long now;
    if (sWaitFirstFrame) {
        sWaitFirstFrame = FALSE;
        now = plat_time_ns();
        if (sLoadWasGame && sPickNs[0] != 0) {
            plat_log("%s; first frame +%d ms; %d ms since A on the track, %d ms since A on the last option", sLine,
                     (int) ((now - sLoadEndNs) / 1000000ULL), (int) ((now - sPickNs[0]) / 1000000ULL),
                     sPickNs[1] != 0 ? (int) ((now - sPickNs[1]) / 1000000ULL) : -1);
            sPickNs[0] = sPickNs[1] = 0;
        } else {
            plat_log("%s; first frame +%d ms", sLine, (int) ((now - sLoadEndNs) / 1000000ULL));
        }
        plat_log_memory("after the load");
        port_gfx_log_cache();
    }
}
