#ifndef PORT_PROFILE_H
#define PORT_PROFILE_H

/*
 * Per-stage frame profiler (port/src/game/port_profile.c). Turned on by DKR_PROFILE=1, the bottom screen's Profiler
 * button, or the unattended test run (DKR_AUTORUN). Only builtin C types, so it can be used from game code, the OS layer and the 3DS backend alike.
 *
 * A frame passes through two threads, which on a New 3DS run on different CPU cores:
 *  - the game thread (main core) runs the game logic, then builds the display list ("scene"), then waits for the
 *    renderer to finish the previous frame and for the next retrace;
 *  - the renderer thread interprets the display list ("rdp", which includes handing the draws to the GPU), decodes
 *    textures, and waits for the GPU to finish the frame before it. The audio tasks run next to it.
 * The GPU's own drawing time for each frame comes from citro3d.
 */

#ifdef __cplusplus
extern "C" {
#endif

enum ProfStage {
    PROF_LOGIC,      /* Game thread: object updates (racers, AI, physics). */
    PROF_SCENE,      /* Game thread: the rest of the frame, mostly building the display list. */
    PROF_FRAME_WAIT, /* Game thread: waiting for the renderer to finish the previous frame. */
    PROF_VSYNC,      /* Game thread: waiting for the retrace that paces the frame rate. */
    PROF_RDP,        /* Renderer: display list interpretation and GPU command submission. */
    PROF_TEXTURE,    /* Renderer: texture decoding and upload. */
    PROF_GPU_WAIT,   /* Renderer: waiting for the GPU to finish the previous frame, or the screen to show it. */
    PROF_AUDIO,      /* Audio tasks (synthesis). */
    PROF_GPU_DRAW,   /* The GPU's drawing time (3DS only). */
    PROF_STAGE_COUNT
};

enum ProfCounter {
    PROF_TRIANGLES,       /* Triangles handed to the backend. */
    PROF_DRAWS,           /* Draw batches handed to the backend. */
    PROF_TEXTURE_UPLOADS, /* Textures decoded (texture cache misses). */
    PROF_COUNTER_COUNT
};

/* Points in the game thread's frame loop (main_game_loop). */
enum ProfMark { PROF_MARK_LOOP_START, PROF_MARK_BUILT, PROF_MARK_WAITED, PROF_MARK_END };

/* Retrace counts per game frame that the frame rate histogram distinguishes (1 = 60 fps ... 6+ = 10 fps or less). */
#define PROF_RATE_BUCKETS 7

int prof_enabled(void);
/* The Settings menu's Profiler row on the bottom screen: on or off, remembered across launches. */
int prof_user_enabled(void);
void prof_set_user_enabled(int on);
void prof_add(int stage, unsigned long long ns);
void prof_count(int counter, unsigned int n);
/* Game thread: called at the marks above. updateRate (retraces this frame took) is only read at PROF_MARK_END. */
void prof_game_mark(int mark, int updateRate);
void prof_logic_begin(void);
void prof_logic_end(void);
/* Renderer: called after each display list, with the whole time it took (PROF_RDP is what the other stages leave). */
void prof_render_task(unsigned long long totalNs);

/*
 * Parts of the renderer's time (PROF_RDP) on the 3DS, for finding where it goes: the backend's draw calls as a whole
 * (what is left of PROF_RDP is the display list walk), and within them the guard band clipping, the GPU settings and
 * the vertex and index writes. Logged with the profile line as averages per rendered frame. Each breakdown has a
 * switch, DKR_DRAW_PARTS for the draws' and DKR_WALK_PARTS for the walk's, off by default on an Old 3DS: there each
 * of their clock reads is a system call, a few thousand a frame, while PROF_RDP needs two.
 */
enum ProfPart {
    PROF_PART_BACKEND,
    PROF_PART_CLIP,
    PROF_PART_STATE,
    PROF_PART_STATE_TWICE, /* Within PROF_PART_STATE: writing the settings a second time (drain_before_draw) */
    PROF_PART_WRITE,
    /*
     * The display list walk (gfx_rdp.c, P26): the rest of PROF_RDP between the start of the display list and the end
     * of the frame, by what it was doing. Each part leaves out the others nested in it, the backend's draws
     * (PROF_PART_BACKEND) and texture decoding (PROF_TEXTURE).
     */
    PROF_PART_WALK_COMMANDS,  /* Reading the commands, and the ones that only set state (modes, tiles, colours) */
    PROF_PART_WALK_MATRICES,  /* G_MTX */
    PROF_PART_WALK_VERTICES,  /* G_VTX: transforms and fog */
    PROF_PART_WALK_TRIANGLES, /* G_TRIN: shared vertex lookup, finished vertices, texture coordinates */
    PROF_PART_WALK_RECTS,     /* Texture and fill rectangles */
    PROF_PART_WALK_DRAWSTATE, /* build_draw_state: blend, combiner, depth settings */
    PROF_PART_WALK_TEXTURES,  /* lookup_texture: hashing the texture and finding it in the cache */
    PROF_PART_WALK_FRAMEEND,  /* Ending the frame (gfxb_end_frame) */
    PROF_PART_WALK_TIMER,     /* The profiler's own clock reads in the walk and the draws, taken out of the parts
                               * and of PROF_RDP */
    /* Waits within the frame end (3DS), shown apart from it: for the GPU to finish the frame before (part of
     * PROF_GPU_WAIT), and for the screen to show it (DKR_PRESENT_WAIT, also counted as PROF_GPU_WAIT). */
    PROF_PART_END_GPU_WAIT,
    PROF_PART_PRESENT_WAIT,
    PROF_PART_COUNT
};
void prof_part(int part, unsigned long long ns);
/* What one plat_ticks() read costs, in 1/256ths of a tick (measured the first time), for taking the profiler's own
 * clock reads back out of the parts they time. */
unsigned long long prof_clock_read_cost(void);

/* Statistics over a span of frames (a race, or the last second for the overlay). */
typedef struct ProfStats {
    unsigned int gameFrames;      /* Frames the game thread produced. */
    unsigned int renderedFrames;  /* Frames the renderer drew. */
    unsigned long long elapsedNs; /* Wall clock time covered. */
    unsigned long long stageNs[PROF_STAGE_COUNT];
    unsigned int counters[PROF_COUNTER_COUNT];
    unsigned int rateFrames[PROF_RATE_BUCKETS]; /* Game frames by retraces taken: [2] = 30 fps, [3] = 20 fps... */
    unsigned int worstFrameUs;    /* Longest time between two game frames. */
    unsigned int worstBuildUs;    /* Longest logic + scene time. */
    unsigned int worstRenderUs;   /* Longest renderer busy time for one frame. */
    unsigned int worstGpuUs;      /* Longest GPU drawing time for one frame. */
    unsigned int hitches;         /* Game frames that took longer than 50 ms. */
    unsigned int low1FrameUs;     /* The 1% low: 99 in 100 game frames came at least this soon (0: no frames). */
    unsigned int pacedFrames[4];  /* Game frames by the time they took: one retrace (60 fps), two (30), three (20), more. */
    unsigned long long idleNs;    /* Main core idle time (3DS with the idle meter, else 0). */
    unsigned long long renderIdleNs; /* The renderer's core (the same core on an Old 3DS). */
} ProfStats;

/* A window of statistics: started with prof_window_start, read (any number of times) with prof_window_read. */
enum ProfWindow { PROF_WINDOW_OVERLAY, PROF_WINDOW_SESSION, PROF_WINDOW_COUNT };
void prof_window_start(int window);
void prof_window_read(int window, ProfStats *out);

/* Which part of the system limits the frame rate, judged from a window's averages: "game CPU", "renderer CPU", "GPU". */
const char *prof_limit(const ProfStats *stats);

/* Extra text shown under the profiler on the bottom screen (the unattended run's progress). NULL clears it. */
void prof_set_status(const char *text);

/*
 * A player's build has no profiler (port_profile.c is left out of RELEASE=1): it is never on, and the calls compile
 * to nothing.
 */
#ifdef PORT_RELEASE
#include <string.h>
#define prof_enabled() 0
#define prof_user_enabled() 0
#define prof_set_user_enabled(on) ((void) (on))
#define prof_add(stage, ns) ((void) (stage), (void) (ns))
#define prof_count(counter, n) ((void) (counter), (void) (n))
#define prof_game_mark(mark, updateRate) ((void) (mark), (void) (updateRate))
#define prof_logic_begin() ((void) 0)
#define prof_logic_end() ((void) 0)
#define prof_render_task(totalNs) ((void) (totalNs))
#define prof_part(part, ns) ((void) (part), (void) (ns))
#define prof_clock_read_cost() 0ULL
#define prof_window_start(window) ((void) (window))
#define prof_window_read(window, out) ((void) (window), memset((out), 0, sizeof(*(out))))
#define prof_limit(stats) ((void) (stats), "-")
#define prof_set_status(text) ((void) (text))
#endif

#ifdef __cplusplus
}
#endif

/* The renderer's draw mix since the last call (gfx_rdp.c): per kind (port_drawtag.h) draws, triangles, vertices. */
void gfx_draw_mix_take(unsigned long out[4][3]);

#endif
