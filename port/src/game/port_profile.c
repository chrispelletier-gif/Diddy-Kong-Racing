/*
 * Per-stage frame profiler (see plat/profile.h for the stages).
 *
 *   DKR_PROFILE=1   Shows the profiler on the bottom screen (updated every second) and logs a summary line every
 *                   few seconds. The Settings menu's Profiler row on the bottom screen does the same, and is
 *                   remembered in profiler.txt in the data directory. The unattended test run (DKR_AUTORUN) profiles
 *                   and logs too, but only shows the readout if one of those is on.
 * While it is off, every hook returns after one check.
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
#include "gfx/gfx_split.h"

#define USER_FILE "profiler.txt"

static s32 sEnabled = -1;      // DKR_PROFILE.
static volatile s32 sUserOn;   // The bottom screen button.
static s32 sGameThreadOn;      // Whether the game thread is profiling (it starts and stops at the top of a frame).

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
    if (!prof_enabled()) {
        return;
    }
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
    if (prof_enabled()) {
        sCounters[counter] += n;
    }
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
    if (!prof_enabled()) {
        return;
    }
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
    if (prof_enabled()) {
        sLogicStartNs = plat_time_ns();
    }
}

void prof_logic_end(void) {
    if (prof_enabled()) {
        sLogicNs += plat_time_ns() - sLogicStartNs;
    }
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

static void show_overlay(void) {
    ProfStats s;
    char text[512];
    u32 slow, i;
    double fps;

    prof_window_read(PROF_WINDOW_OVERLAY, &s);
    fps = s.elapsedNs ? s.gameFrames * 1e9 / s.elapsedNs : 0.0;
    for (slow = 0, i = 4; i < PROF_RATE_BUCKETS; i++) {
        slow += s.rateFrames[i];
    }
    plat_snprintf(text, sizeof(text),
                  "PROFILER %4.1f fps  limit: %s\n"
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
        plat_overlay_show(text);
    } else if (sOverlayShown) {
        // Only a test run is profiling: it still measures and logs, but the readout stays off (as the Settings menu
        // says), so the bottom screen keeps the race HUD.
        sOverlayShown = FALSE;
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
                // What the draws are for (the game's tags, port_drawtag.h): where crowded moments cost the renderer.
                static const char *const sKinds[4] = { "other", "track", "racers", "objects" };
                unsigned long mix[4][3];
                char line[256];
                int k, at = 0;
                gfx_draw_mix_take(mix);
                for (k = 0; k < 4; k++) {
                    at += plat_snprintf(line + at, sizeof(line) - at, "%s%s %lu draws %lu tris %lu verts", k ? ", " : "",
                                        sKinds[k], mix[k][0] / frames, mix[k][1] / frames, mix[k][2] / frames);
                }
                port_log("draw mix a frame: %s", line);
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

void prof_game_mark(int mark, int updateRate) {
    unsigned long long now;
    s32 w;

    if (!prof_enabled()) {
        if (sGameThreadOn) {
            // Turned off: clear the readout, and start afresh if it is turned on again.
            sGameThreadOn = FALSE;
            sWindows[PROF_WINDOW_OVERLAY].startNs = 0;
            sLastFrameEndNs = 0;
            sOverlayShown = FALSE;
            plat_overlay_show("");
        }
        return;
    }
    if (!sGameThreadOn) {
        // Turned on part way through a frame: wait for the next one, so no stage is timed from a stale mark.
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
            break;
        case PROF_MARK_BUILT: {
            unsigned long long build = now - sMarkNs[PROF_MARK_LOOP_START];
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
            if (sWindows[PROF_WINDOW_OVERLAY].startNs == 0) {
                prof_window_start(PROF_WINDOW_OVERLAY);
            } else if (now - sWindows[PROF_WINDOW_OVERLAY].startNs >= 1000000000ULL) {
                show_overlay();
            }
            break;
        }
    }
}
