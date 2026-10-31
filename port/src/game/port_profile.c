/*
 * Per-stage frame profiler (see plat/profile.h for the stages).
 *
 *   DKR_PROFILE=1   Shows the profiler on the bottom screen (updated every second) and logs a summary line every
 *                   few seconds. The Settings menu's Profiler row on the bottom screen does the same, and is
 *                   remembered in profiler.txt in the data directory. The unattended test run (DKR_AUTORUN) profiles
 *                   and logs too, but only shows the readout if one of those is on.
 * While it is off, the stages and counters are still collected (prof_recording) for the flight recorder
 * (port/src/plat/recorder.c), which keeps every frame's row; only the readout, the log lines and the renderer's parts
 * (prof_part, which reads the clock around every draw) wait for it.
 *
 * Each stage has exactly one writer thread, which keeps a running total in microseconds; readers take the difference
 * between two snapshots, so nothing is ever reset under a writer's feet. 32-bit totals are read and written in one
 * go on every target, and wrap after 71 minutes, far longer than any window.
 */
#include <ultra64.h>
#include "types.h"
#include "port.h"
#include "plat/plat.h"
#include "plat/profile.h"
#include "plat/recorder.h"
#include "gfx/gfx_split.h"
#include "port_drawtag.h"
#include "plat/bottom_screen.h"

#define USER_FILE "profiler.txt"

static s32 sEnabled = -1;      // DKR_PROFILE.
static volatile s32 sUserOn;   // The bottom screen button.
static s32 sGameThreadOn;      // Whether the game thread is timing its frames (from the top of one): 2 while the
                               // readout is on too.

/* Running totals (only ever increased, by the stage's own thread). */
static volatile u32 sTotalUs[PROF_STAGE_COUNT];
static unsigned long long sTotalNs[PROF_STAGE_COUNT]; // Private to each stage's writer.
static volatile u32 sCounters[PROF_COUNTER_COUNT];
static volatile u32 sRenderedFrames;

/* Per-window maxima kept by the renderer thread; a window start asks for them to be cleared. */
static volatile u32 sRenderMaxUs[PROF_WINDOW_COUNT];
static volatile u32 sGpuMaxUs[PROF_WINDOW_COUNT];
static volatile u32 sMaxClearRequest[PROF_WINDOW_COUNT];
static u32 sMaxCleared[PROF_WINDOW_COUNT];
/* The renderer's waits and texture work during the current task. */
static unsigned long long sTaskGpuWaitNs;
static unsigned long long sTaskTextureNs;

/* Game thread state. */
static unsigned long long sMarkNs[4];
static unsigned long long sLogicStartNs;
static unsigned long long sLogicNs; // Logic time of the current frame.
static u32 sBuildUs;                // Logic + scene time of the current frame.
static unsigned long long sLastFrameEndNs;
static u32 sGameFrames;

/* The frame time histogram behind the 1% low, and where a frame stops counting as one, two, three or more retraces (half a
 * retrace past each, so a late vblank doesn't move a frame up a group). */
#define PERIOD_BIN_US 500
#define PERIOD_BINS 256
#define PACED_60_US 25000
#define PACED_30_US 41667
#define PACED_20_US 58333

typedef struct Window {
    u32 startTotalUs[PROF_STAGE_COUNT];
    u32 startCounters[PROF_COUNTER_COUNT];
    u32 startRenderedFrames;
    u32 startGameFrames;
    unsigned long long startNs;
    unsigned long long startIdleNs;
    unsigned long long startRenderIdleNs;
    // Game thread statistics, kept per window directly.
    u32 rateFrames[PROF_RATE_BUCKETS];
    u32 worstFrameUs;
    u32 worstBuildUs;
    u32 hitches;
    u32 pacedFrames[4];
    u32 periodBins[PERIOD_BINS]; // Game frame periods in half milliseconds, the last bin taking all longer ones.
} Window;

static Window sWindows[PROF_WINDOW_COUNT];
static char sStatus[96];
static s32 sOverlayShown; // The readout is on the bottom screen.
static s32 sLogCounter;

static void user_file_path(char *path, s32 size) {
    plat_snprintf(path, size, "%s/" USER_FILE, plat_data_dir());
}

/* The first call's read of the Profiler row's file and DKR_PROFILE, out of prof_enabled: the renderer asks it at
 * every draw. */
static PLAT_COLD void read_profile_setting(void) {
    char path[256];
    char text[16];
    int length;
    user_file_path(path, sizeof(path));
    length = plat_load_file_start(path, text, sizeof(text) - 1);
    text[length > 0 ? length : 0] = '\0';
    sUserOn = PLAT_DEBUG_TOOLS && text[0] == '1'; // A player's build has no Profiler row to turn it off.
    sEnabled = plat_getenv("DKR_PROFILE") != NULL;
}

/* On with DKR_PROFILE, the Settings menu's Profiler row, and during the unattended test run (which can start and stop at any time).
 * Out of line: one copy next to the renderer's per-draw code, rather than one in each of its callers. */
PLAT_HOT PLAT_NOINLINE int prof_enabled(void) {
    if (__builtin_expect(sEnabled < 0, 0)) {
        read_profile_setting();
    }
    return sEnabled || sUserOn || port_autorun_enabled();
}

int prof_recording(void) {
    return TRUE;
}

int prof_user_enabled(void) {
    prof_enabled();
    return sUserOn;
}

void prof_set_user_enabled(int on) {
    char path[256];
    prof_enabled();
    sUserOn = on != 0;
    user_file_path(path, sizeof(path));
    plat_save_file(path, on ? "1\n" : "0\n", 2);
}

void prof_add(int stage, unsigned long long ns) {
    if (stage == PROF_GPU_WAIT) {
        if (!gfx_split_active()) { // Else the waits are the second renderer thread's, not the walk's
            sTaskGpuWaitNs += ns;
        }
    } else if (stage == PROF_TEXTURE) {
        sTaskTextureNs += ns;
    }
    sTotalNs[stage] += ns;
    sTotalUs[stage] = (u32) (sTotalNs[stage] / 1000);
    if (stage == PROF_GPU_DRAW) {
        s32 w;
        // Cleared together with the renderer maximum, in prof_render_frame (the same thread).
        for (w = 0; w < PROF_WINDOW_COUNT; w++) {
            if ((u32) (ns / 1000) > sGpuMaxUs[w]) {
                sGpuMaxUs[w] = (u32) (ns / 1000);
            }
        }
    }
}

void prof_count(int counter, unsigned int n) {
    sCounters[counter] += n;
}

static unsigned long long sPartNs[PROF_PART_COUNT];
static unsigned long long sPartLoggedNs[PROF_PART_COUNT];
static u32 sPartLoggedFrames;
static unsigned long long sTaskTimerNs; // This display list's walk clock reads (PROF_PART_WALK_TIMER)

void prof_part(int part, unsigned long long ns) {
    sPartNs[part] += ns;
    if (part == PROF_PART_WALK_TIMER) {
        sTaskTimerNs += ns;
    }
}

unsigned long long prof_clock_read_cost(void) {
    static unsigned long long sCost;
    if (sCost == 0) {
        // The fastest of a few tries, so a thread switch in one doesn't count.
        s32 attempt, i;
        for (attempt = 0; attempt < 8; attempt++) {
            unsigned long long start = plat_ticks(), cost;
            for (i = 0; i < 32; i++) {
                (void) plat_ticks();
            }
            cost = (plat_ticks() - start) * 256 / 33;
            if (attempt == 0 || cost < sCost) {
                sCost = cost;
            }
        }
        if (sCost == 0) {
            sCost = 1;
        }
        port_log("profiler: a clock read takes %llu ns", (sCost * 1000000000ULL / 256) / plat_ticks_per_second());
    }
    return sCost;
}

void prof_render_task(unsigned long long totalNs) {
    unsigned long long busyNs;
    s32 w;
    // Whatever the task spent neither waiting for the GPU nor on textures was display list work.
    busyNs = totalNs > sTaskGpuWaitNs ? totalNs - sTaskGpuWaitNs : 0;
    // Nor timing the walk's parts, so the renderer's time reads the same with or without them.
    busyNs = busyNs > sTaskTimerNs ? busyNs - sTaskTimerNs : 0;
    sTaskTimerNs = 0;
    prof_add(PROF_RDP, busyNs > sTaskTextureNs ? busyNs - sTaskTextureNs : 0);
    sTaskGpuWaitNs = 0;
    sTaskTextureNs = 0;
    sRenderedFrames++;
    for (w = 0; w < PROF_WINDOW_COUNT; w++) {
        u32 request = sMaxClearRequest[w];
        if (sMaxCleared[w] != request) {
            sMaxCleared[w] = request;
            sRenderMaxUs[w] = 0;
            sGpuMaxUs[w] = 0;
        }
        if ((u32) (busyNs / 1000) > sRenderMaxUs[w]) {
            sRenderMaxUs[w] = (u32) (busyNs / 1000);
        }
    }
}

void prof_logic_begin(void) {
    sLogicStartNs = plat_time_ns();
}

void prof_logic_end(void) {
    sLogicNs += plat_time_ns() - sLogicStartNs;
}

void prof_window_start(int window) {
    Window *w = &sWindows[window];
    s32 i;
    for (i = 0; i < PROF_STAGE_COUNT; i++) {
        w->startTotalUs[i] = sTotalUs[i];
    }
    for (i = 0; i < PROF_COUNTER_COUNT; i++) {
        w->startCounters[i] = sCounters[i];
    }
    for (i = 0; i < PROF_RATE_BUCKETS; i++) {
        w->rateFrames[i] = 0;
    }
    bzero(w->pacedFrames, sizeof(w->pacedFrames));
    bzero(w->periodBins, sizeof(w->periodBins));
    w->startRenderedFrames = sRenderedFrames;
    w->startGameFrames = sGameFrames;
    w->startNs = plat_time_ns();
    w->startIdleNs = plat_idle_ns();
    w->startRenderIdleNs = plat_render_idle_ns();
    w->worstFrameUs = 0;
    w->worstBuildUs = 0;
    w->hitches = 0;
    sMaxClearRequest[window]++;
}

/* The frame time that the slowest 1% of a window's game frames took at least: the middle of the half-millisecond bin
 * the 99th percentile lands in. */
static u32 low1_frame_us(const Window *w) {
    u32 total = 0, seen = 0, slowest;
    s32 i;
    for (i = 0; i < PERIOD_BINS; i++) {
        total += w->periodBins[i];
    }
    if (total == 0) {
        return 0;
    }
    slowest = (total + 99) / 100;
    for (i = PERIOD_BINS - 1; i > 0; i--) {
        seen += w->periodBins[i];
        if (seen >= slowest) {
            break;
        }
    }
    return i * PERIOD_BIN_US + PERIOD_BIN_US / 2;
}

void prof_window_read(int window, ProfStats *out) {
    Window *w = &sWindows[window];
    s32 i;
    for (i = 0; i < PROF_STAGE_COUNT; i++) {
        out->stageNs[i] = (unsigned long long) (u32) (sTotalUs[i] - w->startTotalUs[i]) * 1000;
    }
    for (i = 0; i < PROF_COUNTER_COUNT; i++) {
        out->counters[i] = sCounters[i] - w->startCounters[i];
    }
    for (i = 0; i < PROF_RATE_BUCKETS; i++) {
        out->rateFrames[i] = w->rateFrames[i];
    }
    out->gameFrames = sGameFrames - w->startGameFrames;
    out->renderedFrames = sRenderedFrames - w->startRenderedFrames;
    out->elapsedNs = plat_time_ns() - w->startNs;
    out->idleNs = plat_idle_ns() - w->startIdleNs;
    out->renderIdleNs = plat_render_idle_ns() - w->startRenderIdleNs;
    out->worstFrameUs = w->worstFrameUs;
    out->worstBuildUs = w->worstBuildUs;
    out->hitches = w->hitches;
    out->low1FrameUs = low1_frame_us(w);
    for (i = 0; i < 4; i++) {
        out->pacedFrames[i] = w->pacedFrames[i];
    }
    if (sMaxCleared[window] == sMaxClearRequest[window]) {
        out->worstRenderUs = sRenderMaxUs[window];
        out->worstGpuUs = sGpuMaxUs[window];
    } else {
        out->worstRenderUs = 0; // Nothing rendered since the window started.
        out->worstGpuUs = 0;
    }
}

static double per_frame_ms(const ProfStats *s, s32 stage, u32 frames) {
    return frames ? s->stageNs[stage] / 1e6 / frames : 0.0;
}

const char *prof_limit(const ProfStats *s) {
    double game = per_frame_ms(s, PROF_LOGIC, s->gameFrames) + per_frame_ms(s, PROF_SCENE, s->gameFrames);
    double render = per_frame_ms(s, PROF_RDP, s->renderedFrames) + per_frame_ms(s, PROF_TEXTURE, s->renderedFrames);
    double gpu = per_frame_ms(s, PROF_GPU_DRAW, s->renderedFrames);
    if (gpu > game && gpu > render) {
        return "GPU";
    }
    return (game >= render) ? "game CPU" : "renderer CPU";
}

void prof_set_status(const char *text) {
    s32 i = 0;
    if (text != NULL) {
        for (; text[i] != '\0' && i < (s32) sizeof(sStatus) - 1; i++) {
            sStatus[i] = text[i];
        }
    }
    sStatus[i] = '\0';
}

static int percent(u32 part, u32 whole) {
    return whole ? (int) (((unsigned long long) part * 100 + whole / 2) / whole) : 0;
}

/*
 * The draw mix and the object mix (profile.h): the game thread's build time per kind and per object kind, the object
 * logic's time, and the scene counts, as the game draws; taken with the renderer's once a second into the last
 * second's (the monitor and the log lines) and the window's (a test run's race). The clock is only read on timed
 * frames (sGameTimed): while the profiler is on, and on an Old 3DS, whose clock reads are system calls, one frame in 8.
 */
static s32 sGameTimed;
static u32 sGameFrameCount;
static unsigned long long sKindBuildNs[PROF_KINDS];
static unsigned long long sKindStartNs, sOtherAtStartNs;
static s32 sKindNow, sObjNow = PROF_OBJ_NONE;
static unsigned long sGameTimedFrames;
static ProfSceneCounts sScene;
static ProfDrawMix sSecondMix, sWindowMix;
static ProfSceneCounts sSecondScene, sWindowScene;
static ProfObjMix sObjLive, sSecondObj, sWindowObj;

static const char *const sKindNames[PORT_DRAW_KINDS] = {
    "menus/other", "track", "racers", "objects", "sky", "see-through", "effects", "shadows", "HUD",
};

/* objects.h BHV_*, in order. */
static const char *const sObjectNames[PROF_OBJ_PARTICLES] = {
    "none", "racer", "scenery", "fish", "animator", "weapon", "smoke", "exit", "audio", "audio line",
    "camera control", "setup point", "dino whale", "checkpoint", "door", "fog changer", "ainode", "weapon balloon",
    "weapon 2", "audio line 2", "unk 14", "unk 15", "bomb explosion", "balloon pop", "unk 18", "unk 19",
    "sky control", "audio reverb", "torch mist", "texture scroll", "modechange", "stopwatch man", "banana",
    "light rgba", "unk 22", "unk 23", "buoy pirate ship", "weather", "bridge whale ramp", "ramp switch",
    "sea monster", "bonus", "unk 2a", "lens flare", "lens flare switch", "collect egg", "egg creator",
    "character flag", "unk 30", "animation", "animated object", "camera animation", "info point", "car animation",
    "character select", "trigger", "vehicle animation", "zipper water", "timetrial ghost", "wave generator",
    "wave power", "butterfly", "park warden", "unk 3f", "world key", "banana spawner", "treasure sucker", "log",
    "lava spurt", "pos arrow", "hit tester", "midi fade", "hit tester 2", "effect box", "trophy cabinet", "bubbler",
    "fly coin", "golden balloon", "laser bolt", "laser gun", "park warden 2", "animated object 2", "zipper ground",
    "override pos", "wizpig ship", "animated object 3", "animated object 4", "taj telepoint", "silver coin", "boost",
    "warden smoke", "unk 5b", "unk 5c", "zipper air", "unk 5e", "dynamic light object 2", "snowball", "snowball 2",
    "teleport", "rocket signpost", "rocket signpost 2", "snowball 3", "snowball 4", "hit tester 3", "hit tester 4",
    "range trigger", "unk 6a", "boss hazard trigger", "fireball octoweapon", "frog", "silver coin 2", "tt door",
    "midi fade point", "door opener", "unk 72", "pig rocketeer", "fireball octoweapon 2", "level name",
    "midi channel set", "wizpig ghosts", "unk 78", "unk 79", "unk 7a", "unk 7b", "unk 7c", "unk 7d", "unk 7e",
    "unk 7f",
};

const char *prof_kind_name(int kind) {
    return kind >= 0 && kind < PORT_DRAW_KINDS ? sKindNames[kind] : "?";
}

const char *prof_object_name(int bucket) {
    return bucket == PROF_OBJ_PARTICLES ? "particles" : bucket >= 0 && bucket < PROF_OBJ_PARTICLES ? sObjectNames[bucket] : "?";
}

void port_draw_kind_game(int kind, int object) {
    unsigned long long now;
    if (object >= 0 && object < PROF_OBJ_BUCKETS) {
        sObjLive.objs[object].drawn++;
        sScene.objectsDrawn++;
    }
    if (sKindStartNs == 0) {
        return; // Not a timed frame
    }
    now = plat_time_ns();
    sKindBuildNs[sKindNow] += now - sKindStartNs;
    if (sObjNow < PROF_OBJ_BUCKETS) {
        sObjLive.objs[sObjNow].buildNs += now - sKindStartNs;
    }
    sKindStartNs = now;
    sKindNow = kind & (PROF_KINDS - 1);
    sObjNow = object >= 0 && object < PROF_OBJ_BUCKETS ? object : PROF_OBJ_NONE;
}

unsigned long long prof_object_logic_begin(void) {
    return sGameTimed ? plat_time_ns() : 0;
}

void prof_object_logic_end(int bucket, unsigned long long start) {
    ProfObjStats *o = &sObjLive.objs[bucket < PROF_OBJ_BUCKETS ? bucket : 0];
    o->updates++;
    if (start != 0) {
        o->logicNs += plat_time_ns() - start;
    }
}

void prof_kind_build_take(ProfDrawMix *out) {
    s32 k;
    for (k = 0; k < PROF_KINDS; k++) {
        out->kinds[k].buildNs += sKindBuildNs[k];
        sKindBuildNs[k] = 0;
    }
    out->gameTimedFrames += sGameTimedFrames;
    sGameTimedFrames = 0;
}

void prof_scene_frame(int segments, int segmentsAllowed, int segmentsDrawn, int objects) {
    sScene.frames++;
    sScene.segments += segments;
    sScene.segmentsAllowed += segmentsAllowed;
    sScene.segmentsDrawn += segmentsDrawn;
    sScene.objects += objects;
}

void prof_scene_take(ProfSceneCounts *out) {
    *out = sScene;
    bzero(&sScene, sizeof(sScene));
}

static void add_objects(ProfObjMix *to, const ProfObjMix *from) {
    s32 k;
    for (k = 0; k < PROF_OBJ_BUCKETS; k++) {
        ProfObjStats *t = &to->objs[k];
        const ProfObjStats *f = &from->objs[k];
        t->updates += f->updates;
        t->drawn += f->drawn;
        t->tris += f->tris;
        t->logicNs += f->logicNs;
        t->buildNs += f->buildNs;
        t->renderNs += f->renderNs;
    }
    to->frames += from->frames;
    to->timedFrames += from->timedFrames;
    to->renderTimedFrames += from->renderTimedFrames;
}

static void add_mix(ProfDrawMix *to, const ProfDrawMix *from) {
    s32 k;
    for (k = 0; k < PROF_KINDS; k++) {
        ProfKindStats *t = &to->kinds[k];
        const ProfKindStats *f = &from->kinds[k];
        t->draws += f->draws;
        t->tris += f->tris;
        t->verts += f->verts;
        t->renderNs += f->renderNs;
        t->buildNs += f->buildNs;
        t->audited += f->audited;
        t->offscreen += f->offscreen;
        t->backface += f->backface;
        t->tiny += f->tiny;
        t->areaPx += f->areaPx;
    }
    to->frames += from->frames;
    to->timedFrames += from->timedFrames;
    to->auditedFrames += from->auditedFrames;
    to->gameTimedFrames += from->gameTimedFrames;
}

static void add_scene(ProfSceneCounts *to, const ProfSceneCounts *from) {
    to->frames += from->frames;
    to->segments += from->segments;
    to->segmentsAllowed += from->segmentsAllowed;
    to->segmentsDrawn += from->segmentsDrawn;
    to->objects += from->objects;
    to->objectsDrawn += from->objectsDrawn;
}

/* Once a second (show_overlay): the renderer's and the game's draw mix into the last second's and the window's. */
static void take_draw_mix(void) {
    bzero(&sSecondMix, sizeof(sSecondMix));
    gfx_draw_mix_take(&sSecondMix);
    prof_kind_build_take(&sSecondMix);
    prof_scene_take(&sSecondScene);
    add_mix(&sWindowMix, &sSecondMix);
    add_scene(&sWindowScene, &sSecondScene);
    sSecondObj = sObjLive;
    bzero(&sObjLive, sizeof(sObjLive));
    sSecondObj.frames = sSecondMix.frames;
    sSecondObj.timedFrames = sSecondMix.gameTimedFrames;
    sSecondObj.renderTimedFrames = sSecondMix.timedFrames;
    gfx_object_mix_take(&sSecondObj);
    add_objects(&sWindowObj, &sSecondObj);
}

const ProfObjMix *prof_object_mix_last_second(void) {
    return &sSecondObj;
}

const ProfObjMix *prof_object_mix_window(void) {
    return &sWindowObj;
}

void prof_draw_mix_last_second(ProfDrawMix *mix, ProfSceneCounts *scene) {
    *mix = sSecondMix;
    *scene = sSecondScene;
}

void prof_draw_mix_window(ProfDrawMix *mix, ProfSceneCounts *scene, int restart) {
    if (mix != NULL) {
        *mix = sWindowMix;
        *scene = sWindowScene;
    }
    if (restart) {
        bzero(&sWindowMix, sizeof(sWindowMix));
        bzero(&sWindowScene, sizeof(sWindowScene));
        bzero(&sWindowObj, sizeof(sWindowObj));
    }
}

/* Per kind a frame: triangles/draws, renderer/game thread ms, of the audited triangles the share wasted (off screen /
 * facing away / tiny) and the overdraw (screen area drawn over the screen's: x1.00 covers it once); then the scene
 * counts. Short, to fit a log line. */
void prof_format_draw_mix(const ProfDrawMix *m, const ProfSceneCounts *scene, char *out, int size) {
    double frames = m->frames ? (double) m->frames : 1.0, timed = m->timedFrames ? (double) m->timedFrames : 1.0;
    double gameTimed = m->gameTimedFrames ? (double) m->gameTimedFrames : 1.0;
    double audited = m->auditedFrames ? (double) m->auditedFrames : 1.0;
    s32 k, at = 0;
    out[0] = '\0';
    for (k = 0; k < PORT_DRAW_KINDS && at < size; k++) {
        const ProfKindStats *s = &m->kinds[k];
        double a = s->audited ? (double) s->audited : 1.0;
        if (s->draws == 0) {
            continue;
        }
        at += plat_snprintf(out + at, size - at, "%s%s %.0f/%.0f %.2f/%.2f %.0f/%.0f/%.0f%% x%.2f", at ? "; " : "",
                            sKindNames[k], s->tris / frames, s->draws / frames, s->renderNs / 1e6 / timed,
                            s->buildNs / 1e6 / gameTimed, s->offscreen * 100.0 / a, s->backface * 100.0 / a,
                            s->tiny * 100.0 / a, s->areaPx / audited / (400.0 * 240.0));
    }
    if (scene->frames > 0 && at < size) {
        plat_snprintf(out + at, size - at,
                      "; segments %.1f of %.1f (visibility table %.1f), objects %.1f of %.1f",
                      (double) scene->segmentsDrawn / scene->frames, (double) scene->segments / scene->frames,
                      (double) scene->segmentsAllowed / scene->frames, (double) scene->objectsDrawn / scene->frames,
                      (double) scene->objects / scene->frames);
    }
}

/* The costliest object kinds, a frame each: updated/drawn/triangles, logic/build/renderer ms. Short, for a log line. */
void prof_format_object_mix(const ProfObjMix *m, int top, char *out, int size) {
    double frames = m->frames ? (double) m->frames : 1.0, timed = m->timedFrames ? (double) m->timedFrames : 1.0;
    double renderTimed = m->renderTimedFrames ? (double) m->renderTimedFrames : 1.0;
    double cost[PROF_OBJ_BUCKETS];
    u8 picked[PROF_OBJ_BUCKETS];
    s32 k, n, at = 0;
    out[0] = '\0';
    for (k = 0; k < PROF_OBJ_BUCKETS; k++) {
        const ProfObjStats *o = &m->objs[k];
        cost[k] = o->logicNs / timed + o->buildNs / timed + o->renderNs / renderTimed;
        cost[k] += o->tris * 1e-3 / frames; // Orders the untimed by triangles
        picked[k] = o->updates == 0 && o->drawn == 0 && o->tris == 0;
    }
    for (n = 0; n < top && at < size; n++) {
        s32 best = -1;
        for (k = 0; k < PROF_OBJ_BUCKETS; k++) {
            if (!picked[k] && (best < 0 || cost[k] > cost[best])) {
                best = k;
            }
        }
        if (best < 0) {
            break;
        }
        picked[best] = TRUE;
        {
            const ProfObjStats *o = &m->objs[best];
            at += plat_snprintf(out + at, size - at, "%s%s %.0f/%.0f/%.0f %.2f/%.2f/%.2f", at ? "; " : "",
                                prof_object_name(best), o->updates / frames, o->drawn / frames, o->tris / frames,
                                o->logicNs / 1e6 / timed, o->buildNs / 1e6 / timed, o->renderNs / 1e6 / renderTimed);
        }
    }
}

/*
 * The live monitor on the bottom screen while the readout is on: the frame graph (the flight recorder's last frames,
 * each coloured by what took its time) over one of the pages, refreshed once a second. A tap on the graph freezes it
 * (and the page) to read; a tap under it shows the next page.
 */
enum { MON_SUMMARY, MON_KINDS, MON_OBJECTS, MON_WASTE, MON_PAGES };
static volatile s32 sMonPage, sMonFrozen, sMonDirty;
static char sSummaryText[512];
static const char *const sMonPageNames[MON_PAGES] = { "PROFILER", "DRAW KINDS", "OBJECTS", "WASTE" };

int prof_monitor_tap(int x, int y) {
    (void) x;
    if (!sOverlayShown) {
        return 0;
    }
    if (y < BS_GRAPH_BOTTOM) {
        sMonFrozen = !sMonFrozen;
    } else {
        sMonPage = (sMonPage + 1) % MON_PAGES;
    }
    sMonDirty = TRUE;
    return 1;
}

static void show_graph(void) {
    const RecState *rec = rec_state(); // Read in place: a copy would cost an Old 3DS 11 KB.
    unsigned char height[BS_GRAPH_FRAMES], kind[BS_GRAPH_FRAMES];
    u32 count = __atomic_load_n(&rec->frameCount, __ATOMIC_ACQUIRE), first, i;
    s32 n = count < BS_GRAPH_FRAMES ? (s32) count : BS_GRAPH_FRAMES;
    first = count - (u32) n;
    for (i = 0; i < (u32) n; i++) {
        const RecFrame *f = &rec->frames[(first + i) % REC_FRAMES];
        u32 halfMs = (f->periodUs + 250) / 500;
        height[i] = (unsigned char) (halfMs > 255 ? 255 : halfMs);
        kind[i] = (unsigned char) (rec_frame_cause(f) | ((f->flags & REC_FLAG_HITCH) ? BS_GRAPH_HITCH : 0));
    }
    plat_overlay_graph(height, kind, n);
}

static void show_monitor_page(void) {
    static char text[1024];
    const ProfDrawMix *m = &sSecondMix;
    const ProfSceneCounts *sc = &sSecondScene;
    double frames = m->frames ? (double) m->frames : 1.0, timed = m->timedFrames ? (double) m->timedFrames : 1.0;
    double gameTimed = m->gameTimedFrames ? (double) m->gameTimedFrames : 1.0;
    double audited = m->auditedFrames ? (double) m->auditedFrames : 1.0;
    s32 page = sMonPage, at, k;

    at = plat_snprintf(text, sizeof(text), "%s %d/%d%s  tap: page\n", sMonPageNames[page], (int) page + 1,
                       MON_PAGES, sMonFrozen ? " FROZEN" : "");
    switch (page) {
        case MON_SUMMARY:
            plat_snprintf(text + at, sizeof(text) - at, "%sgraph: grn logic cyan build yel rdp\n"
                                                        "org tex red gpu grey retrace; tap: freeze",
                          sSummaryText);
            break;
        case MON_KINDS:
            at += plat_snprintf(text + at, sizeof(text) - at, "a frame:   tris drw  rdp   bld waste\n");
            for (k = 0; k < PORT_DRAW_KINDS && at < (s32) sizeof(text); k++) {
                const ProfKindStats *st = &m->kinds[k];
                double a = st->audited ? (double) st->audited : 1.0;
                if (st->draws == 0) {
                    continue;
                }
                at += plat_snprintf(text + at, sizeof(text) - at, "%-9.9s %5.0f %3.0f %5.2f %5.2f %3.0f%%\n",
                                    sKindNames[k], st->tris / frames, st->draws / frames, st->renderNs / 1e6 / timed,
                                    st->buildNs / 1e6 / gameTimed,
                                    (st->offscreen + st->backface + st->tiny) * 100.0 / a);
            }
            if (sc->frames > 0 && at < (s32) sizeof(text)) {
                plat_snprintf(text + at, sizeof(text) - at, "segments %.0f/%.0f (table %.0f)\nobjects %.0f/%.0f drawn",
                              (double) sc->segmentsDrawn / sc->frames, (double) sc->segments / sc->frames,
                              (double) sc->segmentsAllowed / sc->frames, (double) sc->objectsDrawn / sc->frames,
                              (double) sc->objects / sc->frames);
            }
            break;
        case MON_OBJECTS: {
            const ProfObjMix *o = &sSecondObj;
            double of = o->frames ? (double) o->frames : 1.0, ot = o->timedFrames ? (double) o->timedFrames : 1.0;
            double ort = o->renderTimedFrames ? (double) o->renderTimedFrames : 1.0;
            u8 picked[PROF_OBJ_BUCKETS];
            s32 n;
            at += plat_snprintf(text + at, sizeof(text) - at, "a frame:   upd drw  tris logic  bld  rdp\n");
            for (k = 0; k < PROF_OBJ_BUCKETS; k++) {
                picked[k] = o->objs[k].updates == 0 && o->objs[k].drawn == 0 && o->objs[k].tris == 0;
            }
            for (n = 0; n < 13 && at < (s32) sizeof(text); n++) {
                s32 best = -1;
                double bestCost = -1.0;
                for (k = 0; k < PROF_OBJ_BUCKETS; k++) {
                    const ProfObjStats *b = &o->objs[k];
                    double cost = (b->logicNs + b->buildNs) / ot + b->renderNs / ort + b->tris * 1e-3;
                    if (!picked[k] && cost > bestCost) {
                        best = k;
                        bestCost = cost;
                    }
                }
                if (best < 0) {
                    break;
                }
                picked[best] = TRUE;
                at += plat_snprintf(text + at, sizeof(text) - at, "%-9.9s %3.0f %3.0f %5.0f %5.2f %4.2f %4.2f\n",
                                    prof_object_name(best), o->objs[best].updates / of, o->objs[best].drawn / of,
                                    o->objs[best].tris / of, o->objs[best].logicNs / 1e6 / ot,
                                    o->objs[best].buildNs / 1e6 / ot, o->objs[best].renderNs / 1e6 / ort);
            }
            break;
        }
        default:
            at += plat_snprintf(text + at, sizeof(text) - at, "triangles: off  back tiny overdraw\n");
            for (k = 0; k < PORT_DRAW_KINDS && at < (s32) sizeof(text); k++) {
                const ProfKindStats *st = &m->kinds[k];
                double a = st->audited ? (double) st->audited : 1.0;
                if (st->draws == 0) {
                    continue;
                }
                at += plat_snprintf(text + at, sizeof(text) - at, "%-9.9s %4.0f%% %4.0f%% %3.0f%% x%.2f\n",
                                    sKindNames[k], st->offscreen * 100.0 / a, st->backface * 100.0 / a,
                                    st->tiny * 100.0 / a, st->areaPx / audited / (400.0 * 240.0));
            }
            if (at < (s32) sizeof(text)) {
                plat_snprintf(text + at, sizeof(text) - at, "off: past an edge; back: GPU culls it;\n"
                                                            "tiny: under a pixel; x1 = screen once");
            }
            break;
    }
    plat_overlay_show(text);
}

static void show_overlay(void) {
    ProfStats s;
    u32 slow, i;
    double fps;

    prof_window_read(PROF_WINDOW_OVERLAY, &s);
    take_draw_mix();
    fps = s.elapsedNs ? s.gameFrames * 1e9 / s.elapsedNs : 0.0;
    for (slow = 0, i = 4; i < PROF_RATE_BUCKETS; i++) {
        slow += s.rateFrames[i];
    }
    plat_snprintf(sSummaryText, sizeof(sSummaryText),
                  "%4.1f fps  limit: %s\n"
                  "30fps %3d%%  20fps %3d%%  slower %3d%%\n"
                  "worst frame %3u ms  hitches %u\n"
                  "game  logic %4.1f scene %4.1f wait %4.1f\n"
                  "rend  rdp %4.1f tex %4.1f gpuwait %4.1f\n"
                  "gpu   draw %4.1f  audio %4.1f\n"
                  "idle  game core %2d%%  render core %2d%%\n"
                  "tris %5u draws %4u tex+ %u\n"
                  "%s",
                  fps, prof_limit(&s), percent(s.rateFrames[2] + s.rateFrames[1], s.gameFrames),
                  percent(s.rateFrames[3], s.gameFrames), percent(slow, s.gameFrames), s.worstFrameUs / 1000,
                  s.hitches, per_frame_ms(&s, PROF_LOGIC, s.gameFrames), per_frame_ms(&s, PROF_SCENE, s.gameFrames),
                  per_frame_ms(&s, PROF_FRAME_WAIT, s.gameFrames), per_frame_ms(&s, PROF_RDP, s.renderedFrames),
                  per_frame_ms(&s, PROF_TEXTURE, s.renderedFrames), per_frame_ms(&s, PROF_GPU_WAIT, s.renderedFrames),
                  per_frame_ms(&s, PROF_GPU_DRAW, s.renderedFrames), per_frame_ms(&s, PROF_AUDIO, s.gameFrames),
                  percent((u32) (s.idleNs / 1000), (u32) (s.elapsedNs / 1000)),
                  percent((u32) (s.renderIdleNs / 1000), (u32) (s.elapsedNs / 1000)),
                  s.renderedFrames ? s.counters[PROF_TRIANGLES] / s.renderedFrames : 0,
                  s.renderedFrames ? s.counters[PROF_DRAWS] / s.renderedFrames : 0, s.counters[PROF_TEXTURE_UPLOADS],
                  sStatus);
    if (sEnabled || sUserOn) {
        sOverlayShown = TRUE;
        if (!sMonFrozen) {
            show_graph();
            show_monitor_page();
        }
    } else if (sOverlayShown) {
        // Only a test run is profiling: it still measures and logs, but the readout stays off (as the Settings menu
        // says), so the bottom screen keeps the race HUD.
        sOverlayShown = FALSE;
        plat_overlay_graph(NULL, NULL, 0);
        plat_overlay_show("");
    }
    if (++sLogCounter % 4 == 0) {
        static unsigned long long sScreenLoggedNs;
        static unsigned long sScreenLoggedDraws;
        static u32 sScreenLoggedFrames;
        unsigned long long screenNs = plat_screen_busy_ns();
        unsigned long screenDraws = plat_screen_draws();
        u32 frames = sRenderedFrames - sScreenLoggedFrames;
        if (screenDraws != sScreenLoggedDraws && frames > 0) {
            // P36: what drawing the bottom screen costs (on an Old 3DS it shares the game's core).
            port_log("bottom screen: %.2f ms a frame at most, %.1f areas drawn a frame",
                     (screenNs - sScreenLoggedNs) / 1e6 / frames, (double) (screenDraws - sScreenLoggedDraws) / frames);
        }
        sScreenLoggedNs = screenNs;
        sScreenLoggedDraws = screenDraws;
        sScreenLoggedFrames = sRenderedFrames;
        port_log("profile: %.1f fps (30fps %d%%), worst %u ms, logic %.2f scene %.2f rdp %.2f tex %.2f gpu %.2f "
                 "audio %.2f ms, limit %s, idle game core %d%% render core %d%%",
                 fps, percent(s.rateFrames[2] + s.rateFrames[1], s.gameFrames), s.worstFrameUs / 1000,
                 per_frame_ms(&s, PROF_LOGIC, s.gameFrames), per_frame_ms(&s, PROF_SCENE, s.gameFrames),
                 per_frame_ms(&s, PROF_RDP, s.renderedFrames), per_frame_ms(&s, PROF_TEXTURE, s.renderedFrames),
                 per_frame_ms(&s, PROF_GPU_DRAW, s.renderedFrames), per_frame_ms(&s, PROF_AUDIO, s.gameFrames),
                 prof_limit(&s), percent((u32) (s.idleNs / 1000), (u32) (s.elapsedNs / 1000)),
                 percent((u32) (s.renderIdleNs / 1000), (u32) (s.elapsedNs / 1000)));
        {
            // The renderer's parts since the last log line (the counters are the renderer's, read without a lock:
            // a part caught mid-update is off by one draw). The backend's are only timed on the 3DS, and either
            // breakdown can be off (DKR_WALK_PARTS, DKR_DRAW_PARTS: the Old 3DS's defaults, as their clock reads are
            // system calls there), which leaves the renderer's time above whole.
            u32 frames = sRenderedFrames - sPartLoggedFrames;
            double perFrame[PROF_PART_COUNT];
            s32 p, walkTimed = FALSE;
            s32 backendTimed = sPartNs[PROF_PART_BACKEND] != sPartLoggedNs[PROF_PART_BACKEND];
            for (p = 0; p < PROF_PART_COUNT; p++) {
                unsigned long long now = sPartNs[p];
                perFrame[p] = frames ? (now - sPartLoggedNs[p]) / 1e6 / frames : 0.0;
                if (p >= PROF_PART_WALK_COMMANDS && p <= PROF_PART_WALK_FRAMEEND && now != sPartLoggedNs[p]) {
                    walkTimed = TRUE;
                }
                sPartLoggedNs[p] = now;
            }
            sPartLoggedFrames = sRenderedFrames;
            if (backendTimed) {
                port_log("renderer parts: draws %.2f ms (clip %.2f, settings %.2f of which written twice %.2f, "
                         "vertices %.2f), %u tris %u draws a frame",
                         perFrame[PROF_PART_BACKEND], perFrame[PROF_PART_CLIP], perFrame[PROF_PART_STATE],
                         perFrame[PROF_PART_STATE_TWICE], perFrame[PROF_PART_WRITE],
                         s.renderedFrames ? s.counters[PROF_TRIANGLES] / s.renderedFrames : 0,
                         s.renderedFrames ? s.counters[PROF_DRAWS] / s.renderedFrames : 0);
            }
            if (frames > 0) {
                char line[1024];
                prof_format_draw_mix(&sSecondMix, &sSecondScene, line, sizeof(line));
                port_log("draw mix a frame (tris/draws rdp/build ms, waste off/back/tiny, overdraw): %s", line);
                prof_format_object_mix(&sSecondObj, 10, line, sizeof(line));
                port_log("objects a frame (updated/drawn/tris logic/build/rdp ms): %s", line);
                if (gfx_split_active()) {
                    unsigned long long walkWait, busy;
                    unsigned int backendFrames, wakes;
                    gfx_split_take_times(&walkWait, &busy, &backendFrames, &wakes);
                    port_log("two-core renderer: backend busy %.2f ms a frame, woken %.1f times a frame; the walk "
                             "waited %.2f ms a frame for it",
                             backendFrames ? busy / 1e6 / backendFrames : 0.0,
                             backendFrames ? (double) wakes / backendFrames : 0.0, walkWait / 1e6 / frames);
                }
            }
            // The frame end's waits are shown apart from its work.
            perFrame[PROF_PART_WALK_FRAMEEND] -=
                perFrame[PROF_PART_END_GPU_WAIT] + perFrame[PROF_PART_PRESENT_WAIT];
            if (perFrame[PROF_PART_WALK_FRAMEEND] < 0.0) {
                perFrame[PROF_PART_WALK_FRAMEEND] = 0.0;
            }
            if (!walkTimed) {
                port_log("%s: not timed in parts (DKR_WALK_PARTS=0%s), waits at the frame end: GPU %.2f, screen "
                         "%.2f ms, timer %.2f ms taken out",
                         backendTimed ? "display list walk" : "renderer parts",
                         backendTimed ? "" : ", DKR_DRAW_PARTS=0", perFrame[PROF_PART_END_GPU_WAIT],
                         perFrame[PROF_PART_PRESENT_WAIT], perFrame[PROF_PART_WALK_TIMER]);
            } else {
                port_log("display list walk: %.2f ms (commands %.2f, matrices %.2f, vertices %.2f, triangles %.2f, "
                         "rectangles %.2f, draw state %.2f, texture lookups %.2f, frame end %.2f), waits at the frame "
                         "end: GPU %.2f, screen %.2f ms, timer %.2f ms taken out",
                         perFrame[PROF_PART_WALK_COMMANDS] + perFrame[PROF_PART_WALK_MATRICES] +
                             perFrame[PROF_PART_WALK_VERTICES] + perFrame[PROF_PART_WALK_TRIANGLES] +
                             perFrame[PROF_PART_WALK_RECTS] + perFrame[PROF_PART_WALK_DRAWSTATE] +
                             perFrame[PROF_PART_WALK_TEXTURES] + perFrame[PROF_PART_WALK_FRAMEEND],
                         perFrame[PROF_PART_WALK_COMMANDS], perFrame[PROF_PART_WALK_MATRICES],
                         perFrame[PROF_PART_WALK_VERTICES], perFrame[PROF_PART_WALK_TRIANGLES],
                         perFrame[PROF_PART_WALK_RECTS], perFrame[PROF_PART_WALK_DRAWSTATE],
                         perFrame[PROF_PART_WALK_TEXTURES], perFrame[PROF_PART_WALK_FRAMEEND],
                         perFrame[PROF_PART_END_GPU_WAIT], perFrame[PROF_PART_PRESENT_WAIT],
                         perFrame[PROF_PART_WALK_TIMER]);
            }
        }
    }
    prof_window_start(PROF_WINDOW_OVERLAY);
}

/* The flight recorder's row for the frame just ended: the game thread's stages, and what the renderer, the GPU and
 * the sound did since the row before (from the running totals). */
static void record_frame(u32 periodUs, u32 buildUs, int updateRate, unsigned long long now) {
    static u32 sLastTotalUs[PROF_STAGE_COUNT], sLastCounters[PROF_COUNTER_COUNT];
    u32 totals[PROF_STAGE_COUNT], counters[PROF_COUNTER_COUNT];
    u32 logicUs = (u32) (sLogicNs / 1000);
    RecFrame row;
    s32 i;

    for (i = 0; i < PROF_STAGE_COUNT; i++) {
        totals[i] = sTotalUs[i];
    }
    for (i = 0; i < PROF_COUNTER_COUNT; i++) {
        counters[i] = sCounters[i];
    }
    bzero(&row, sizeof(row));
    row.periodUs = periodUs;
    row.logicUs = logicUs;
    row.sceneUs = buildUs > logicUs ? buildUs - logicUs : 0;
    row.renderWaitUs = (u32) ((sMarkNs[PROF_MARK_WAITED] - sMarkNs[PROF_MARK_BUILT]) / 1000);
    row.vsyncUs = (u32) ((now - sMarkNs[PROF_MARK_WAITED]) / 1000);
    row.rdpUs = totals[PROF_RDP] - sLastTotalUs[PROF_RDP];
    row.texUs = totals[PROF_TEXTURE] - sLastTotalUs[PROF_TEXTURE];
    row.gpuUs = totals[PROF_GPU_DRAW] - sLastTotalUs[PROF_GPU_DRAW];
    row.gpuWaitUs = totals[PROF_GPU_WAIT] - sLastTotalUs[PROF_GPU_WAIT];
    row.audioUs = totals[PROF_AUDIO] - sLastTotalUs[PROF_AUDIO];
    row.tris = counters[PROF_TRIANGLES] - sLastCounters[PROF_TRIANGLES];
    row.draws = (unsigned short) (counters[PROF_DRAWS] - sLastCounters[PROF_DRAWS]);
    row.texDecodes = (unsigned short) (counters[PROF_TEXTURE_UPLOADS] - sLastCounters[PROF_TEXTURE_UPLOADS]);
    row.rate = (signed char) updateRate;
    row.flags = prof_enabled() ? REC_FLAG_PROFILER : 0;
    for (i = 0; i < PROF_STAGE_COUNT; i++) {
        sLastTotalUs[i] = totals[i];
    }
    for (i = 0; i < PROF_COUNTER_COUNT; i++) {
        sLastCounters[i] = counters[i];
    }
    rec_game_frame(&row);
    if (sMonDirty && sOverlayShown) {
        // A tap on the monitor: show it now rather than at the next second.
        sMonDirty = FALSE;
        show_monitor_page();
    }
}

void prof_game_mark(int mark, int updateRate) {
    unsigned long long now;
    s32 w, shown = prof_enabled();

    if (!shown && sGameThreadOn == 2) {
        // Turned off: clear the readout, and start its window afresh if it is turned on again.
        sGameThreadOn = TRUE;
        sWindows[PROF_WINDOW_OVERLAY].startNs = 0;
        sOverlayShown = FALSE;
        plat_overlay_graph(NULL, NULL, 0);
        plat_overlay_show("");
    }
    if (!sGameThreadOn) {
        // The first frame: start at its top, so no stage is timed from a stale mark.
        if (mark != PROF_MARK_LOOP_START) {
            return;
        }
        sGameThreadOn = TRUE;
    }
    now = plat_time_ns();
    sMarkNs[mark] = now;
    switch (mark) {
        case PROF_MARK_LOOP_START:
            sLogicNs = 0;
            sKindNow = PORT_DRAW_OTHER;
            sObjNow = PROF_OBJ_NONE;
            sGameFrameCount++;
            sGameTimed = prof_enabled() && (!plat_old_3ds() || sGameFrameCount % 8 == 0);
            sGameTimedFrames += sGameTimed;
            sKindStartNs = sGameTimed ? now : 0;
            sOtherAtStartNs = sKindBuildNs[PORT_DRAW_OTHER];
            break;
        case PROF_MARK_BUILT: {
            unsigned long long build = now - sMarkNs[PROF_MARK_LOOP_START];
            sBuildUs = (u32) (build / 1000);
            if (sKindStartNs != 0) {
                // The last kind ends here; the logic ran under "menus/other" and is shown apart.
                unsigned long long other;
                port_draw_kind_game(PORT_DRAW_OTHER, -1);
                other = sKindBuildNs[PORT_DRAW_OTHER];
                other -= other >= sOtherAtStartNs ? sOtherAtStartNs : 0; // (A take may have emptied it since.)
                sKindBuildNs[PORT_DRAW_OTHER] -= other > sLogicNs ? sLogicNs : other;
                sKindStartNs = 0;
            }
            prof_add(PROF_LOGIC, sLogicNs);
            prof_add(PROF_SCENE, build > sLogicNs ? build - sLogicNs : 0);
            for (w = 0; w < PROF_WINDOW_COUNT; w++) {
                if (build / 1000 > sWindows[w].worstBuildUs) {
                    sWindows[w].worstBuildUs = (u32) (build / 1000);
                }
            }
            break;
        }
        case PROF_MARK_WAITED:
            prof_add(PROF_FRAME_WAIT, now - sMarkNs[PROF_MARK_BUILT]);
            break;
        case PROF_MARK_END: {
            u32 periodUs = sLastFrameEndNs ? (u32) ((now - sLastFrameEndNs) / 1000) : 0;
            prof_add(PROF_VSYNC, now - sMarkNs[PROF_MARK_WAITED]);
            sLastFrameEndNs = now;
            sGameFrames++;
            if (updateRate < 0) {
                updateRate = 0;
            }
            if (updateRate >= PROF_RATE_BUCKETS) {
                updateRate = PROF_RATE_BUCKETS - 1;
            }
            for (w = 0; w < PROF_WINDOW_COUNT; w++) {
                Window *win = &sWindows[w];
                win->rateFrames[updateRate]++;
                if (periodUs > win->worstFrameUs) {
                    win->worstFrameUs = periodUs;
                }
                if (periodUs > 50000) {
                    win->hitches++;
                }
                if (periodUs != 0) {
                    win->periodBins[periodUs / PERIOD_BIN_US < PERIOD_BINS ? periodUs / PERIOD_BIN_US : PERIOD_BINS - 1]++;
                    win->pacedFrames[periodUs < PACED_60_US ? 0 : periodUs < PACED_30_US ? 1 : periodUs < PACED_20_US ? 2 : 3]++;
                }
            }
            record_frame(periodUs, sBuildUs, updateRate, now);
            if (!shown) {
                break;
            }
            sGameThreadOn = 2; // The readout is in use.
            if (sWindows[PROF_WINDOW_OVERLAY].startNs == 0) {
                prof_window_start(PROF_WINDOW_OVERLAY);
            } else if (now - sWindows[PROF_WINDOW_OVERLAY].startNs >= 1000000000ULL) {
                show_overlay();
            }
            break;
        }
    }
}
