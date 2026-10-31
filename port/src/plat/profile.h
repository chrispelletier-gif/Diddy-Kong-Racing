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
/*
 * Whether the stages and counters are being collected: always in a debug build, for the flight recorder
 * (plat/recorder.h), whether or not the profiler's readout is on. Only the renderer's parts (prof_part) wait for
 * prof_enabled.
 */
int prof_recording(void);
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
#define prof_recording() 0
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

/*
 * The draw mix: per kind of thing drawn (port_drawtag.h PORT_DRAW_*), what it cost and how much of it was wasted.
 * Counts are since the last take. The renderer's and the game thread's times are only taken while the profiler is on
 * (on an Old 3DS, one frame in 8), the waste only on audited frames (one in 8 while the profiler or a test run is on):
 * a triangle is off the screen if all its corners are past one edge or behind the camera, facing away if it is one
 * sided and its back is to the camera (the GPU culls it), tiny if it covers less than a pixel; the rest add their area
 * on the screen (an overestimate for those cut by an edge), which over the screen's gives the overdraw.
 */
#define PROF_KINDS 16
typedef struct ProfKindStats {
    unsigned long draws, tris, verts;
    unsigned long long renderNs;  /* The renderer's time from this kind's tag to the next. */
    unsigned long long buildNs;   /* The game thread's (port_profile.c fills it in). */
    unsigned long audited;        /* Triangles looked at on audited frames, and of those: */
    unsigned long offscreen, backface, tiny;
    float areaPx;                 /* Screen area of the rest, in top screen pixels (400 x 240). */
} ProfKindStats;
typedef struct ProfDrawMix {
    ProfKindStats kinds[PROF_KINDS];
    unsigned long frames, timedFrames, auditedFrames;
    unsigned long gameTimedFrames; /* Frames the game thread timed its building in (buildNs). */
} ProfDrawMix;
/* Adds the renderer's draw mix since the last call to out (gfx_rdp.c), and starts afresh. */
void gfx_draw_mix_take(ProfDrawMix *out);
/* The names of the kinds ("track", "racers"...). */
const char *prof_kind_name(int kind);
/* Adds the game thread's build time per kind since the last call to out, and starts afresh (port_profile.c). */
void prof_kind_build_take(ProfDrawMix *out);
/* What the game drew: level segments of all, those the visibility table allowed, and of those the ones in view and
 * drawn; objects active (tracks.c, once a view) and drawn (counted from their tags). Read with prof_scene_take: totals
 * since the last call. */
typedef struct ProfSceneCounts {
    unsigned long frames, segments, segmentsAllowed, segmentsDrawn, objects, objectsDrawn;
} ProfSceneCounts;
void prof_scene_frame(int segments, int segmentsAllowed, int segmentsDrawn, int objects);
void prof_scene_take(ProfSceneCounts *out);
/* The draw mix and scene counts of the last whole second (while the profiler or a test run is on). */
void prof_draw_mix_last_second(ProfDrawMix *mix, ProfSceneCounts *scene);
/* Those since the window was last restarted (mix NULL: only restarts it). A test run restarts it with each race. */
void prof_draw_mix_window(ProfDrawMix *mix, ProfSceneCounts *scene, int restart);
/* One line: per kind a frame the triangles, draws, times, waste and overdraw; then the scene counts. */
void prof_format_draw_mix(const ProfDrawMix *mix, const ProfSceneCounts *scene, char *out, int size);
/*
 * The object mix: per kind of object (its behaviour, objects.h BHV_*; particles together) how many were updated and
 * drawn, their triangles, and while the profiler is on (an Old 3DS: one frame in 8) the game logic's, the game
 * thread's display list building's and the renderer's time on them. The renderer's share comes from the object's tag
 * (port_drawtag.h port_draw_tag_object). Kept apart from the draw mix: it is bigger.
 */
#define PROF_OBJ_PARTICLES 128
#define PROF_OBJ_BUCKETS 129
#define PROF_OBJ_NONE PROF_OBJ_BUCKETS /* Draws that are no object's (the renderer's spare slot). */
typedef struct ProfObjStats {
    unsigned long updates, drawn, tris;
    unsigned long long logicNs, buildNs, renderNs;
} ProfObjStats;
typedef struct ProfObjMix {
    ProfObjStats objs[PROF_OBJ_BUCKETS];
    unsigned long frames, timedFrames, renderTimedFrames; /* timedFrames: the game thread's, for logic and build. */
} ProfObjMix;
/* Adds the renderer's triangles and time per object kind since the last call to out, and starts afresh (gfx_rdp.c). */
void gfx_object_mix_take(ProfObjMix *out);
/* The object kind's name ("racer", "scenery"...). */
const char *prof_object_name(int bucket);
/* Game thread, around an object's update (objects.c): returns the start for the end. */
unsigned long long prof_object_logic_begin(void);
void prof_object_logic_end(int bucket, unsigned long long start);
/* The object mix of the last whole second, and of the draw mix's window (restarted with it). Game thread. */
const ProfObjMix *prof_object_mix_last_second(void);
const ProfObjMix *prof_object_mix_window(void);
/* The `top` costliest object kinds (by game logic + building + renderer time, else triangles), a frame each. */
void prof_format_object_mix(const ProfObjMix *mix, int top, char *out, int size);
/* A tap on the profiler's readout (any thread): on the graph freezes or unfreezes it, below it shows the next page.
 * Returns nonzero if it took the tap. */
int prof_monitor_tap(int x, int y);
#ifdef PORT_RELEASE
#define prof_monitor_tap(x, y) ((void) (x), (void) (y), 0)
#define prof_scene_frame(segments, allowed, drawn, objects) ((void) 0)
#define prof_object_logic_begin() 0ULL
#define prof_object_logic_end(bucket, start) ((void) (start))
#endif

#endif
