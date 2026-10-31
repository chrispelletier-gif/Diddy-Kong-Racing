/* The flight recorder (see recorder.h). Left out of a player's build. */
#ifndef PORT_RELEASE
#include "recorder.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"

/* One retrace, in microseconds (the 3DS screen refreshes at 59.83 Hz). */
#define RETRACE_US 16716

extern const char *volatile gPortGameStage; // port.h, which is for game code

static RecState sRec;
static volatile unsigned int sPressPending;
static volatile int sDescribeRequest, sDescribeDone;
static char sDescribeText[512];

static unsigned int now_ms(void) {
    return (unsigned int) (plat_time_ns() / 1000000ULL);
}

static void init(void) {
    if (sRec.magic != REC_MAGIC) {
        sRec.version = REC_VERSION;
        sRec.size = sizeof(sRec);
        snprintf(sRec.build, sizeof(sRec.build), "%s", plat_build_version());
        sRec.magic = REC_MAGIC;
    }
}

/* What took a frame's time: the game thread's biggest stage, or what the renderer it waited for was doing. */
int rec_frame_cause(const RecFrame *r) {
    if (r->logicUs >= r->sceneUs && r->logicUs >= r->renderWaitUs && r->logicUs >= r->vsyncUs) {
        return REC_CAUSE_LOGIC;
    }
    if (r->sceneUs >= r->renderWaitUs && r->sceneUs >= r->vsyncUs) {
        return REC_CAUSE_SCENE;
    }
    if (r->renderWaitUs >= r->vsyncUs) {
        if (r->gpuWaitUs > r->rdpUs + r->texUs) {
            return REC_CAUSE_GPU;
        }
        return r->texUs > r->rdpUs ? REC_CAUSE_TEXTURES : REC_CAUSE_RENDERER;
    }
    return REC_CAUSE_RETRACE;
}

const char *rec_cause_name(int cause) {
    static const char *const sNames[REC_CAUSE_COUNT] = {
        "game logic", "game thread (scene, loads, sound queue)", "renderer", "textures", "GPU", "retrace",
    };
    return cause >= 0 && cause < REC_CAUSE_COUNT ? sNames[cause] : "?";
}

static void describe_cause(const RecFrame *r, char *out, size_t size) {
    switch (rec_frame_cause(r)) {
        case REC_CAUSE_LOGIC:
            snprintf(out, size, "game logic %u ms", r->logicUs / 1000);
            break;
        case REC_CAUSE_SCENE:
            snprintf(out, size, "game thread outside the object updates (scene, loads, sound queue) %u ms",
                     r->sceneUs / 1000);
            break;
        case REC_CAUSE_RETRACE:
            snprintf(out, size, "waiting %u ms for the retrace (the frame was ready; the screen side was late)",
                     r->vsyncUs / 1000);
            break;
        default:
            snprintf(out, size,
                     "waiting %u ms for the renderer (display list %u ms, textures %u ms / %u decoded, waiting for the "
                     "GPU %u ms, GPU drawing %u ms)",
                     r->renderWaitUs / 1000, r->rdpUs / 1000, r->texUs / 1000, r->texDecodes, r->gpuWaitUs / 1000,
                     r->gpuUs / 1000);
            break;
    }
}

static const char *mode_name(int mode);
static const RecEvent *event_at(const RecState *s, unsigned int n);

/* A hitch, as it happens: its cause and the events during it (at most three), kept in the hitch ring. */
static void record_hitch(const RecFrame *r) {
    RecHitch *h = &sRec.hitches[sRec.hitchCount % REC_HITCHES];
    unsigned int start = r->endMs - r->periodUs / 1000, n, shown = 0, first;
    int length;
    char cause[200];

    h->frame = r->frame;
    h->endMs = r->endMs;
    h->periodUs = r->periodUs;
    h->cause = rec_frame_cause(r);
    describe_cause(r, cause, sizeof(cause));
    length = snprintf(h->text, sizeof(h->text), "%s level %d%s: %s", mode_name(r->mode), r->level,
                      r->racing ? " racing" : "", cause);
    if (r->soundGaps && length < (int) sizeof(h->text)) {
        length += snprintf(h->text + length, sizeof(h->text) - length, "; %u sound gaps", r->soundGaps);
    }
    first = sRec.eventCount > 32 ? sRec.eventCount - 32 : 0;
    for (n = first; n < sRec.eventCount && shown < 3 && length < (int) sizeof(h->text); n++) {
        const RecEvent *ev = event_at(&sRec, n);
        if (ev != NULL && (int) (ev->ms - start) >= -5 && (int) (ev->ms - r->endMs) <= 5 &&
            strncmp(ev->text, "watchdog", 8) != 0) {
            length += snprintf(h->text + length, sizeof(h->text) - length, "%s%.60s", shown == 0 ? "; during: " : " | ",
                               ev->text);
            shown++;
        }
    }
    sRec.causeCounts[h->cause]++;
    sRec.hitchCount++;
}

static void write_text(const char *name, const char *text) {
    char path[256];
    if (text != NULL) {
        snprintf(path, sizeof(path), "%s/%s", plat_data_dir(), name);
        plat_save_file(path, text, strlen(text));
    }
}

/*
 * DKR_REC_TEST=n (for checking the recorder on a PC): presses the shortcut at game frame n, and 30 frames later writes
 * rec_timeline.csv, rec_summary.txt and rec_game.txt to the data directory as a bug report would have them.
 */
static void test_hook(unsigned int frame) {
    static int sTestFrame = -2;
    if (sTestFrame == -2) {
        const char *value = plat_getenv("DKR_REC_TEST");
        sTestFrame = value != NULL ? atoi(value) : -1;
    }
    if (sTestFrame < 0) {
        return;
    }
    if (frame == (unsigned int) sTestFrame) {
        rec_press("test shortcut (DKR_REC_TEST)");
    } else if (frame == (unsigned int) sTestFrame + 30) {
        char *csv = rec_format_csv(&sRec), *summary = rec_format_summary(&sRec), game[512];
        port_rec_describe(game, sizeof(game));
        write_text("rec_timeline.csv", csv);
        write_text("rec_summary.txt", summary);
        write_text("rec_game.txt", game);
        free(csv);
        free(summary);
        plat_log("recorder test: wrote rec_timeline.csv, rec_summary.txt and rec_game.txt");
    }
}

void rec_game_frame(RecFrame *row) {
    static unsigned int sLastGaps;
    unsigned int gaps = gPlatCounters[PLAT_COUNTER_AUDIO_UNDERRUN] + gPlatCounters[PLAT_COUNTER_AUDIO_DROPPED];
    RecFrame *slot;

    init();
    row->frame = sRec.frameCount;
    row->renderFrame = gPlatCounters[PLAT_COUNTER_GFX_DONE];
    row->endMs = now_ms();
    row->soundGaps = (unsigned short) (gaps - sLastGaps);
    sLastGaps = gaps;
    sRec.targetUs = plat_frame_retraces() * RETRACE_US;
    {
        // A hitch is a frame half as long again as usual: the cap's period, or the recent pace when the game runs
        // slower than the cap (at 60 fps a GPU-bound track runs at 30, which the results' pacing columns show).
        static unsigned int sPaceUs;
        unsigned int usual = sPaceUs > sRec.targetUs ? sPaceUs : sRec.targetUs;
        if (row->frame > 0 && row->periodUs * 2 > usual * 3) {
            row->flags |= REC_FLAG_HITCH;
        }
        if (row->periodUs < 4 * sRec.targetUs) {
            sPaceUs = sPaceUs - sPaceUs / 16 + row->periodUs / 16;
        }
    }
    if (sPressPending) {
        sPressPending = 0;
        row->flags |= REC_FLAG_PRESS;
        sRec.pressFrame = row->frame;
    }
    port_rec_game_state(row);
    if (row->flags & REC_FLAG_HITCH) {
        record_hitch(row);
    }
    slot = &sRec.frames[sRec.frameCount % REC_FRAMES];
    *slot = *row;
    __atomic_store_n(&sRec.frameCount, sRec.frameCount + 1, __ATOMIC_RELEASE);

    test_hook(row->frame);
    if (sDescribeRequest) {
        port_rec_describe(sDescribeText, sizeof(sDescribeText));
        sDescribeRequest = 0;
        __atomic_store_n(&sDescribeDone, 1, __ATOMIC_RELEASE);
    }
}

static void add_event(const char *prefix, const char *text) {
    unsigned int n = __atomic_fetch_add(&sRec.eventCount, 1, __ATOMIC_RELAXED);
    RecEvent *e = &sRec.events[n % REC_EVENTS];
    __atomic_store_n(&e->seq, 0, __ATOMIC_RELEASE);
    e->ms = now_ms();
    e->frame = sRec.frameCount;
    snprintf(e->text, sizeof(e->text), "%s%s", prefix, text);
    {
        // One line each: a multi-line log entry keeps its first line.
        char *newline = strchr(e->text, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }
    }
    __atomic_store_n(&e->seq, n + 1, __ATOMIC_RELEASE);
}

void rec_event(const char *fmt, ...) {
    char text[REC_EVENT_TEXT];
    va_list args;
    init();
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    add_event("", text);
}

void rec_log_line(const char *prefix, const char *text) {
    init();
    add_event(prefix, text);
}

void rec_press(const char *what) {
    init();
    sRec.pressMs = now_ms();
    sPressPending = 1;
    rec_event(">>> %s", what);
}

const RecState *rec_state(void) {
    init();
    return &sRec;
}

void rec_copy(RecState *out) {
    unsigned int frameCount;
    init();
    // The count from before the copy: a frame row written while it was copied is not counted as whole.
    frameCount = __atomic_load_n(&sRec.frameCount, __ATOMIC_ACQUIRE);
    memcpy(out, &sRec, sizeof(*out));
    out->frameCount = frameCount;
}

int rec_describe_game(char *out, int size, int timeoutMs) {
    int waited;
    __atomic_store_n(&sDescribeDone, 0, __ATOMIC_RELEASE);
    sDescribeRequest = 1;
    for (waited = 0; waited < timeoutMs && !__atomic_load_n(&sDescribeDone, __ATOMIC_ACQUIRE); waited += 10) {
        plat_sleep_ns(10000000ULL);
    }
    if (!sDescribeDone) {
        sDescribeRequest = 0;
        snprintf(out, size, "(the game thread did not answer within %d ms: %s)", timeoutMs,
                 gPortGameStage != NULL ? gPortGameStage : "-");
        return 0;
    }
    snprintf(out, size, "%s", sDescribeText);
    return 1;
}

unsigned int rec_frame_now(void) {
    return sRec.frameCount;
}

unsigned int rec_hitches_since(unsigned int since, char *out, int size, unsigned int *counts) {
    unsigned int end = sRec.hitchCount, n = end > REC_HITCHES ? end - REC_HITCHES : 0, found = 0, written = 0;
    int length = 0;
    if (size > 0) {
        out[0] = '\0';
    }
    for (; n < end; n++) {
        const RecHitch *h = &sRec.hitches[n % REC_HITCHES];
        if (h->frame < since) {
            continue;
        }
        found++;
        if (counts != NULL && h->cause < REC_CAUSE_COUNT) {
            counts[h->cause]++;
        }
        // Room is kept for the line saying how many more there were.
        if (length < size - 300) {
            length += snprintf(out + length, size - length, "- frame %u at %u ms: **%u ms**, %s\n", h->frame, h->endMs,
                               h->periodUs / 1000, h->text);
            written++;
        }
    }
    if (found > written && length < size) {
        snprintf(out + length, size - length, "- ... and %u more\n", found - written);
    }
    return found;
}

/* Formatting. */

typedef struct Buf {
    char *data;
    size_t length, capacity;
} Buf;

static void add(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add(Buf *b, const char *fmt, ...) {
    va_list args;
    int n;
    if (b->data == NULL) {
        return;
    }
    for (;;) {
        va_start(args, fmt);
        n = vsnprintf(b->data + b->length, b->capacity - b->length, fmt, args);
        va_end(args);
        if (n < 0) {
            return;
        }
        if (b->length + n < b->capacity) {
            b->length += n;
            return;
        }
        {
            char *grown = realloc(b->data, b->capacity * 2 + n);
            if (grown == NULL) {
                free(b->data);
                b->data = NULL;
                return;
            }
            b->data = grown;
            b->capacity = b->capacity * 2 + n;
        }
    }
}

/* The rings' whole entries, oldest first: the index of the first and how many. */
static unsigned int first_frame(const RecState *s, unsigned int *count) {
    *count = s->frameCount < REC_FRAMES ? s->frameCount : REC_FRAMES;
    return s->frameCount - *count;
}

static unsigned int first_event(const RecState *s, unsigned int *count) {
    *count = s->eventCount < REC_EVENTS ? s->eventCount : REC_EVENTS;
    return s->eventCount - *count;
}

static const RecEvent *event_at(const RecState *s, unsigned int n) {
    const RecEvent *e = &s->events[n % REC_EVENTS];
    return e->seq == n + 1 ? e : NULL;
}

static const char *mode_name(int mode) {
    switch (mode) {
        case -1:
            return "intro";
        case 0:
            return "in-game";
        case 1:
            return "menu";
        case 5:
            return "lockup";
        default:
            return "other";
    }
}

/* Whether the last press is within the recording: one from long before (an earlier bug report, before a test run's
 * report) is left out. */
static int press_recorded(const RecState *s) {
    unsigned int count, first = first_frame(s, &count);
    return s->pressMs != 0 && count > 0 && s->pressMs >= s->frames[first % REC_FRAMES].endMs;
}

/* Times relative to the press, or to the last frame if there was none in the recording. */
static unsigned int reference_ms(const RecState *s) {
    unsigned int count;
    first_frame(s, &count);
    if (press_recorded(s)) {
        return s->pressMs;
    }
    return count > 0 ? s->frames[(s->frameCount - 1) % REC_FRAMES].endMs : 0;
}

static void add_csv_text(Buf *b, const char *text) {
    add(b, "\"");
    for (; *text != '\0'; text++) {
        add(b, *text == '"' ? "\"\"" : "%c", *text);
    }
    add(b, "\"");
}

#define MS(us) ((us) / 1000), ((us) / 10 % 100)

char *rec_format_csv(const RecState *s) {
    Buf b;
    unsigned int frameCount, eventCount, f, e, fEnd, eEnd;
    unsigned int ref = reference_ms(s);

    b.capacity = 160 * 1024;
    b.length = 0;
    b.data = malloc(b.capacity);
    if (b.data == NULL) {
        return NULL;
    }
    b.data[0] = '\0';
    f = first_frame(s, &frameCount);
    e = first_event(s, &eventCount);
    fEnd = f + frameCount;
    eEnd = e + eventCount;
    add(&b, "# DKR flight recorder, build %s; frame rate cap %u.%02u ms; %s\n", s->build, MS(s->targetUs),
        press_recorded(s) ? "rel_ms is the time from the bug report shortcut (>>> row)" : "rel_ms is from the last frame");
    add(&b, "# kind f = a game frame (times in ms; renderer/GPU/sound columns are their work since the row before), "
            "e = an event, flags: 1 hitch, 2 shortcut pressed, 4 profiler on\n");
    add(&b, "kind,frame,render_frame,t_ms,rel_ms,period_ms,logic_ms,scene_ms,render_wait_ms,vsync_ms,rdp_ms,tex_ms,"
            "gpu_ms,gpu_wait_ms,audio_ms,tris,draws,tex_decodes,sound_gaps,rate,mode,level,menu,paused,racing,lap,cp,"
            "flags,text\n");
    while (f < fEnd || e < eEnd) {
        const RecFrame *row = f < fEnd ? &s->frames[f % REC_FRAMES] : NULL;
        const RecEvent *ev = NULL;
        while (e < eEnd && (ev = event_at(s, e)) == NULL) {
            e++; // Half written when it was copied.
        }
        if (e >= eEnd) {
            ev = NULL;
        }
        if (ev != NULL && (row == NULL || (int) (ev->ms - row->endMs) <= 0)) {
            add(&b, "e,%u,,%u,%d,,,,,,,,,,,,,,,,,,,,,,,,", ev->frame, ev->ms, (int) (ev->ms - ref));
            add_csv_text(&b, ev->text);
            add(&b, "\n");
            e++;
            continue;
        }
        if (row == NULL) {
            break; // Only half-written events were left.
        }
        add(&b,
            "f,%u,%u,%u,%d,%u.%02u,%u.%02u,%u.%02u,%u.%02u,%u.%02u,%u.%02u,%u.%02u,%u.%02u,%u.%02u,%u.%02u,%u,%u,%u,%u,"
            "%d,%s,%d,%d,%u,%u,%u,%u,%u,\n",
            row->frame, row->renderFrame, row->endMs, (int) (row->endMs - ref), MS(row->periodUs), MS(row->logicUs),
            MS(row->sceneUs), MS(row->renderWaitUs), MS(row->vsyncUs), MS(row->rdpUs), MS(row->texUs),
            MS(row->gpuUs), MS(row->gpuWaitUs), MS(row->audioUs), row->tris, row->draws, row->texDecodes,
            row->soundGaps, row->rate, mode_name(row->mode), row->level, row->menu, row->paused, row->racing, row->lap,
            row->checkpoint, row->flags);
        f++;
    }
    return b.data;
}

#define WORST 8

char *rec_format_summary(const RecState *s) {
    Buf b;
    unsigned int frameCount, eventCount, f0, e0, i, j;
    unsigned int ref = reference_ms(s), hitches = 0, worstIdx[WORST], worstN = 0;
    unsigned int beforeFrames = 0, beforeWorst = 0, beforeHitches = 0;
    unsigned long long beforeUs = 0;

    b.capacity = 8 * 1024;
    b.length = 0;
    b.data = malloc(b.capacity);
    if (b.data == NULL) {
        return NULL;
    }
    b.data[0] = '\0';
    f0 = first_frame(s, &frameCount);
    e0 = first_event(s, &eventCount);
    if (frameCount == 0) {
        add(&b, "No frames recorded.\n");
        return b.data;
    }
    for (i = f0; i < f0 + frameCount; i++) {
        const RecFrame *r = &s->frames[i % REC_FRAMES];
        int rel = (int) (r->endMs - ref);
        if (r->flags & REC_FLAG_HITCH) {
            hitches++;
        }
        if (rel <= 0 && rel > -2000) {
            beforeFrames++;
            beforeUs += r->periodUs;
            beforeWorst = r->periodUs > beforeWorst ? r->periodUs : beforeWorst;
            beforeHitches += (r->flags & REC_FLAG_HITCH) != 0;
        }
    }
    // The WORST longest frames, each at least 6 frames from a longer one already picked, so one slow stretch shows
    // once (the first frame's period runs from start-up, so it is left out).
    while (worstN < WORST) {
        unsigned int best = 0, found = 0;
        for (i = f0; i < f0 + frameCount; i++) {
            const RecFrame *r = &s->frames[i % REC_FRAMES];
            int near = 0;
            if (r->frame == 0 || (found && r->periodUs <= s->frames[best % REC_FRAMES].periodUs)) {
                continue;
            }
            for (j = 0; j < worstN && !near; j++) {
                near = i + 6 > worstIdx[j] && worstIdx[j] + 6 > i;
            }
            if (!near) {
                best = i;
                found = 1;
            }
        }
        if (!found || s->frames[best % REC_FRAMES].periodUs * 2 <= s->targetUs * 3) {
            break; // Only hitches are listed.
        }
        worstIdx[worstN++] = best;
    }
    {
        const RecFrame *first = &s->frames[f0 % REC_FRAMES], *last = &s->frames[(f0 + frameCount - 1) % REC_FRAMES];
        add(&b, "Recorded %u frames over %.1f s (frame rate cap %u ms a frame); %u hitches (frames half as long again as "
            "the usual pace).\n",
            frameCount, (last->endMs - first->endMs) / 1000.0, s->targetUs / 1000, hitches);
    }
    if (press_recorded(s)) {
        add(&b, "Shortcut pressed at %u ms, game frame %u. In the 2 s before it: %u frames (%.1f fps), slowest %u ms, "
                "%u hitches.\n",
            s->pressMs, s->pressFrame, beforeFrames, beforeUs ? beforeFrames * 1e6 / beforeUs : 0.0,
            beforeWorst / 1000, beforeHitches);
    }
    if (s->hitchCount > 0) {
        int c;
        add(&b, "Hitches since start-up: %u (", s->hitchCount);
        for (c = 0; c < REC_CAUSE_COUNT; c++) {
            add(&b, "%s%s %u", c > 0 ? ", " : "", rec_cause_name(c), s->causeCounts[c]);
        }
        add(&b, ").\n");
    }
    add(&b, "\nSlowest frames (time from the %s; what took the time; what happened then):\n\n",
        press_recorded(s) ? "press" : "last frame");
    for (i = 0; i < worstN; i++) {
        const RecFrame *r = &s->frames[worstIdx[i] % REC_FRAMES];
        unsigned int start = r->endMs - r->periodUs / 1000, shown = 0, n;
        char cause[200];
        describe_cause(r, cause, sizeof(cause));
        add(&b, "- frame %u at %+.2f s, %s level %d menu %d%s: **%u ms**: %s", r->frame,
            (int) (r->endMs - ref) / 1000.0, mode_name(r->mode), r->level, r->menu, r->paused ? " paused" : "",
            r->periodUs / 1000, cause);
        if (r->soundGaps) {
            add(&b, "; %u sound gaps", r->soundGaps);
        }
        for (n = e0; n < e0 + eventCount && shown < 3; n++) {
            const RecEvent *ev = event_at(s, n);
            // Events during the frame, or within 5 ms of its ends.
            if (ev != NULL && (int) (ev->ms - start) >= -5 && (int) (ev->ms - r->endMs) <= 5) {
                add(&b, "%s`%.70s`", shown == 0 ? "; during: " : ", ", ev->text);
                shown++;
            }
        }
        add(&b, "\n");
    }
    if (s->hitchCount > 0) {
        unsigned int n = s->hitchCount > 10 ? s->hitchCount - 10 : 0;
        add(&b, "\nThe last %u hitches (the hitch log reaches back further than the frames):\n\n", s->hitchCount - n);
        for (; n < s->hitchCount; n++) {
            const RecHitch *h = &s->hitches[n % REC_HITCHES];
            add(&b, "- frame %u at %+.2f s: **%u ms**, %s\n", h->frame, (int) (h->endMs - ref) / 1000.0,
                h->periodUs / 1000, h->text);
        }
    }
    return b.data;
}
#endif
