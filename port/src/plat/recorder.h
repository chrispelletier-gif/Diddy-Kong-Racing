#ifndef PORT_RECORDER_H
#define PORT_RECORDER_H

/*
 * The flight recorder (port/src/game/port_recorder.c): always on in a debug build, so a bug report has what led up
 * to a problem even when the Profiler row was off.
 *
 *  - Every game frame: its time and length, the game thread's stages (logic, scene, waiting for the renderer and the
 *    retrace), the renderer's and the GPU's work since the frame before, triangles, draws, texture decodes, sound
 *    gaps, and the game's state (mode, level, menu, pause, lap and checkpoint). The last REC_FRAMES of them.
 *  - Events: every log line (except a snapshot's draw lines), settings changes, the watchdog's counters, and the
 *    moment a bug report was asked for. The last REC_EVENTS of them.
 *
 * Both rings live in one plain struct (RecState), so a crash or a hang can write it to the SD card as it is and the
 * next start can still turn it into timeline.csv. Costs a few clock reads and one row of stores per frame.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define REC_MAGIC 0x52524B44 /* "DKRR" */
#define REC_VERSION 1
#define REC_FRAMES 1024
#define REC_EVENTS 512
#define REC_EVENT_TEXT 100
#define REC_HITCHES 256
#define REC_HITCH_TEXT 200

/* RecFrame.flags */
#define REC_FLAG_HITCH 1   /* Took more than 1.5 times the usual frame (the cap's period, or the recent pace if slower). */
#define REC_FLAG_PRESS 2   /* The bug report shortcut was pressed during this frame. */
#define REC_FLAG_PROFILER 4 /* The detailed profiler was on (its own clock reads add a little to rdp). */

typedef struct RecFrame {
    unsigned int frame;       /* Game frames since start-up. */
    unsigned int renderFrame; /* Frames the renderer had finished by then. */
    unsigned int endMs;       /* When the frame ended, in ms since start-up. */
    unsigned int periodUs;    /* Since the previous frame ended. */
    unsigned int logicUs, sceneUs, renderWaitUs, vsyncUs; /* Game thread. */
    unsigned int rdpUs, texUs, gpuUs, gpuWaitUs, audioUs; /* Renderer, GPU and sound since the previous row. */
    unsigned int tris;
    unsigned short draws, texDecodes, soundGaps, flags;
    signed char rate, mode, level, menu;
    unsigned char paused, racing, lap, checkpoint;
} RecFrame;

typedef struct RecEvent {
    unsigned int seq; /* Its number + 1 once written, so a half-written event can be told apart. */
    unsigned int ms;
    unsigned int frame; /* The game frame it happened during. */
    char text[REC_EVENT_TEXT];
} RecEvent;

/* What took a hitch's time (the biggest part of it). */
enum RecCause {
    REC_CAUSE_LOGIC,    /* Game logic: object updates. */
    REC_CAUSE_SCENE,    /* The rest of the game thread: building the display list, loads, the sound queue. */
    REC_CAUSE_RENDERER, /* Waiting for the renderer's display list work. */
    REC_CAUSE_TEXTURES, /* Waiting for the renderer, mostly decoding textures. */
    REC_CAUSE_GPU,      /* Waiting for the renderer while it waited for the GPU. */
    REC_CAUSE_RETRACE,  /* The frame was ready; waiting for the retrace (the screen side, or the scheduler). */
    REC_CAUSE_COUNT
};

/* A hitch, written as it happened (the events around it would be gone from the ring by the time a long test run
 * reads it). */
typedef struct RecHitch {
    unsigned int frame, endMs, periodUs, cause;
    char text[REC_HITCH_TEXT];
} RecHitch;

typedef struct RecState {
    unsigned int magic, version, size;
    unsigned int frameCount; /* Frames ever recorded; the ring holds the last REC_FRAMES. */
    unsigned int eventCount;
    unsigned int pressFrame, pressMs; /* The last bug report shortcut (0 if none). */
    unsigned int targetUs;            /* The frame rate cap's period when the last frame was recorded. */
    char build[48];
    RecFrame frames[REC_FRAMES];
    RecEvent events[REC_EVENTS];
    unsigned int hitchCount; /* Hitches ever; the ring holds the last REC_HITCHES. */
    unsigned int causeCounts[REC_CAUSE_COUNT];
    RecHitch hitches[REC_HITCHES];
} RecState;

#ifndef PORT_RELEASE
/*
 * Game thread, at the end of each frame (port_profile.c), with the timings and counters filled in; the recorder adds
 * the frame numbers, the time, the sound gaps, the flags and the game's state.
 */
void rec_game_frame(RecFrame *row);
/* Game side (port_recorder.c): the game's state for a row, and in words for a report. Game thread only. */
void port_rec_game_state(RecFrame *row);
void port_rec_describe(char *out, int size);
/* Any thread: a line on the timeline (printf style). Never waits and never touches the SD card. */
void rec_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* The log's copy of each line (plat_3ds.c / plat_posix.c). */
void rec_log_line(const char *prefix, const char *text);
/* The bug report shortcut: marks the frame and the timeline. */
void rec_press(const char *what);
/* The recorder's state as it is now (for writing it out raw, from a crash handler too). */
const RecState *rec_state(void);
/* A copy, safe to read while the game goes on. */
void rec_copy(RecState *out);
/*
 * Turns a state into timeline.csv: one row per frame and per event, oldest first, the press marked. Returns a malloc'd
 * string (NULL if out of memory).
 */
char *rec_format_csv(const RecState *state);
/* A short readable summary for a report's body: the press, the last 2 seconds, and the slowest frames with what took
 * the time and what happened then. Returns a malloc'd string. */
char *rec_format_summary(const RecState *state);
/*
 * The game's state in words ("in a race on Ancient Lake, Diddy in a car, lap 2 ..."), filled in by the game thread at
 * the end of its next frame. Waits up to timeoutMs; returns 0 (and says why in out) if the game thread didn't answer.
 */
int rec_describe_game(char *out, int size, int timeoutMs);
/*
 * The hitches from game frame `since` on, oldest first, one "- ..." line each, into out (at most size bytes; a line
 * saying how many more there were if they don't fit). Adds each one's cause to counts (REC_CAUSE_COUNT entries) when
 * it isn't NULL. Returns how many the ring had (a long stretch of hitches can push the oldest out).
 */
unsigned int rec_hitches_since(unsigned int since, char *out, int size, unsigned int *counts);
/* A cause's name ("game logic", "textures"...). */
const char *rec_cause_name(int cause);
/* Frames recorded so far (the next frame's number). */
unsigned int rec_frame_now(void);
#else
#define rec_game_frame(row) ((void) (row))
#define rec_event(...) ((void) 0)
#define rec_log_line(prefix, text) ((void) (prefix), (void) (text))
#define rec_press(what) ((void) (what))
#endif

#ifdef __cplusplus
}
#endif

#endif
