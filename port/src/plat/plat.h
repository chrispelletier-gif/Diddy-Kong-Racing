#ifndef PORT_PLAT_H
#define PORT_PLAT_H

/*
 * Platform abstraction used by the N64 OS emulation layer.
 * This header deliberately uses only builtin C types so it can be included both from files that
 * use the libultra headers and from files that use the platform's own SDK headers.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Whether the build has the debug tools (profiler, test runs, bug reports, updates, the reference renderer) and its
 * Settings menus the performance switches. A player's build (make RELEASE=1) leaves the tools out altogether (see the
 * Makefile and src/release/) and the switches keep their defaults.
 */
#ifdef PORT_RELEASE
#define PLAT_DEBUG_TOOLS 0
#else
#define PLAT_DEBUG_TOOLS 1
#endif

/*
 * Code layout for the Old 3DS's 16 KB instruction cache (roadmap O9). The renderer's per-draw functions are PLAT_HOT:
 * on ARM they are put in the .text.hot section, which the 3DS link places together, after the rest of the code, so
 * that a draw runs through one compact stretch of code rather than functions spread among cold ones. (A section
 * rather than GCC's hot attribute, which would also have the compiler optimise them harder and inline more into
 * them. On other targets the macro is empty: a section attribute would keep GCC from splitting those functions' own
 * cold blocks off, which it does there.) What never runs in a race frame but sits in the same functions (logging,
 * snapshots, diagnostics, settings read once) is PLAT_COLD, in a function of its own: optimised for size, put in
 * .text.unlikely, and its call taken as the branch not taken. Rare code that still has to be fast, such as decoding a
 * texture at load time, is only kept out of line (PLAT_NOINLINE).
 */
#ifdef __arm__
#define PLAT_HOT __attribute__((section(".text.hot")))
#else
#define PLAT_HOT
#endif
#define PLAT_COLD __attribute__((cold, noinline))
#define PLAT_NOINLINE __attribute__((noinline))

typedef struct PlatThread PlatThread;
typedef void (*PlatThreadFunc)(void *arg);

/* Which kind of thread is being created. Used to pick a CPU core and priority band. */
enum PlatThreadKind {
    PLAT_THREAD_GAME,     /* An emulated N64 thread. Priority follows the N64 priority. */
    PLAT_THREAD_HARDWARE, /* Emulated hardware (VI retrace, PI). Always above every game thread. */
    PLAT_THREAD_RCP,      /* Emulated RDP (graphics). May run on a different core. */
    PLAT_THREAD_AUDIO,    /* Emulated RSP audio. May run on a different core. */
    PLAT_THREAD_RCP2,     /* The renderer's second half (two-core renderer, gfx_split.c): on a core the RDP's isn't. */
};

void plat_init(void);
void plat_shutdown(void);

PlatThread *plat_thread_create(PlatThreadFunc func, void *arg, int n64Priority, int kind, const char *name);
void plat_thread_set_priority(PlatThread *thread, int n64Priority);
/* Blocks the calling thread forever. Used for N64 threads that end in an idle loop. */
void plat_thread_park(void) __attribute__((noreturn));

/* Thread local "current OSThread" pointer. */
void plat_set_current_os_thread(void *thread);
void *plat_get_current_os_thread(void);

/*
 * Time the application core (the game's own threads) has spent idle, and the same for the renderer's core (core 2 on
 * a New 3DS, where the renderer and the sound run; the application core again elsewhere). Only measured on the 3DS;
 * 0 elsewhere.
 */
unsigned long long plat_idle_ns(void);
unsigned long long plat_render_idle_ns(void);
/* The time the bottom screen's own thread has spent drawing (ns), and how many areas it has drawn (P36: on an Old
 * 3DS it shares the game's core). 0 where there is no such thread. */
unsigned long long plat_screen_busy_ns(void);
/* Nonzero on an Old 3DS (roadmap O8): some switches default differently there. */
int plat_old_3ds(void);
unsigned long plat_screen_draws(void);

/* Monotonic time. */
unsigned long long plat_time_ns(void);
/* The raw clock behind plat_time_ns, without its conversion: for timing many short spans (the profiler's parts). */
unsigned long long plat_ticks(void);
unsigned long long plat_ticks_per_second(void);
void plat_sleep_ns(unsigned long long ns);

/* The global "kernel" lock and condition variable that protect every emulated message queue. */
void plat_klock(void);
void plat_kunlock(void);
void plat_kwait(void);
/* Waits with a timeout. Returns 0 if woken, 1 on timeout. */
int plat_kwait_timeout(unsigned long long ns);
void plat_kbroadcast(void);
/* Roadmap P32: the same, on one of a few condition variables picked by key (a message queue), so a message wakes only
 * the threads waiting on its queue (and any that share its variable). Kernel lock held. */
int plat_kwait_on_timeout(const void *key, unsigned long long ns);
void plat_kbroadcast_on(const void *key);
#define PLAT_KEYED_CONDS 32
#define PLAT_KEY_SLOT(key) ((unsigned) (((unsigned long) (key) >> 3) ^ ((unsigned long) (key) >> 9)) % PLAT_KEYED_CONDS)

/* A non recursive mutex that emulates "interrupts disabled". Ownership is tracked by the OS layer. */
void plat_intlock_lock(void);
void plat_intlock_unlock(void);

/* Simple counting semaphore used to hand work to the RCP thread. */
typedef struct PlatSem PlatSem;
PlatSem *plat_sem_create(int initial);
void plat_sem_post(PlatSem *sem);
void plat_sem_wait(PlatSem *sem);
/* Takes the semaphore if it can without waiting: nonzero if it did. */
int plat_sem_try_wait(PlatSem *sem);

/* Waits for the next display refresh (60Hz). */
void plat_wait_vblank(void);

/* Returns nonzero while the application should keep running. Called from the main thread. */
int plat_main_loop_iteration(void);

/* Whole file helpers. plat_load_file returns a malloc'd buffer (or NULL) and writes the size to *size. */
void *plat_load_file(const char *path, unsigned int *size);
int plat_save_file(const char *path, const void *data, unsigned int size);
/* Adds data to the end of a file, making it if there is none. Returns 0 if anything failed. */
int plat_append_file(const char *path, const void *data, unsigned int size);
/* Reads up to size bytes from the start of a file. Returns the number read, or -1 if it cannot be opened. */
int plat_load_file_start(const char *path, void *data, unsigned int size);
void *plat_malloc(size_t size);
void plat_free(void *ptr);
int plat_snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Returns an environment variable / configuration value, or NULL. */
const char *plat_getenv(const char *name);

/*
 * Experimental switches (roadmap P29): a number from the environment / env.txt, or fallback when it isn't set, unless
 * the switch bench has overridden it for a race with plat_override_switch (value -1 drops the override, and
 * plat_clear_switch_overrides drops them all). gPlatSwitchEpoch counts the changes, so code that keeps a switch's
 * value reads it again when the epoch moves on (the renderer does at the start of each frame).
 */
/* Out of line: a switch's name comparisons are not to be inlined into the renderer's code (plat_fov_tan_scale reads
 * one, from the vertex load's billboard placing). */
int plat_switch(const char *name, int fallback) PLAT_NOINLINE;
void plat_override_switch(const char *name, int value);
void plat_clear_switch_overrides(void);
extern volatile int gPlatSwitchEpoch;

/* Scripted input (DKR_INPUT) for automated tests. Returns nonzero when a script is set. */
int plat_input_script(int poll, unsigned short *buttons, signed char *stickX, signed char *stickY);

/* Lists the entries of a directory into names (maxNames entries of nameSize bytes). Returns the count. */
int plat_list_dir(const char *dir, char *names, int nameSize, int maxNames);
/* The same, only the entries whose names start with prefix. */
int plat_list_dir_prefix(const char *dir, const char *prefix, char *names, int nameSize, int maxNames);

/* NULL terminated list of directories to search for a ROM with any name. */
const char *const *plat_rom_search_dirs(void);

/* Directory where the ROM and save files live. */
const char *plat_data_dir(void);

/* Logging is cold: the code around a log call is laid out as the branch not taken (PLAT_HOT says why). */
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2), cold));
/* A start-up progress line: shown on the bottom screen on the 3DS, and logged. */
void plat_status(const char *fmt, ...) __attribute__((format(printf, 1, 2), cold));
void plat_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn, cold));
/* Keeps logging after the start-up lines, which the 3DS otherwise stops at to spare the SD card. */
void plat_log_keep(void) __attribute__((cold));
/* Also writes log lines to this file until called with NULL, which closes it (3DS only). */
void plat_log_copy_to(const char *path) __attribute__((cold));

/*
 * 3DS only: closing the application. The game's threads keep running until the process ends, so the renderer stops
 * at a quit point first (saying whether the GPU was idle), and the process ends without freeing their memory.
 */
enum { PLAT_3DS_PARKED_GPU_IDLE = 1, PLAT_3DS_PARKED_GPU_BUSY = 2 };
void plat_3ds_quit_point(int parkedState);
/* Whether the renderer is to stop at its next quit point (closing or suspending): see gfxb_end_frame. */
int plat_3ds_renderer_should_park(void);
void plat_3ds_pause_renderer(int pause);
/* Keeps the console awake when the lid closes while held (counted: each hold needs a release). */
void plat_3ds_hold_sleep(int hold);
void plat_3ds_exit(void) __attribute__((noreturn));

/*
 * Pauses the game as if START was pressed (3DS: when the HOME menu opens or the lid closes). The request is set from
 * the system's thread and taken once by the game thread at the start of its next frame.
 */
void plat_request_game_pause(void);
/* Ends the game the normal way, from any thread (after a save restore, or the update's "Quit to finish"). */
void plat_request_quit(void);
int plat_take_game_pause(void);

/*
 * The game's own menu sounds for taps on the 3DS bottom screen's menu: requested from that screen's thread, played by
 * the game thread at the start of its next frame (thread3_main.c).
 */
enum { PLAT_UI_SOUND_NONE, PLAT_UI_SOUND_SELECT, PLAT_UI_SOUND_MOVE, PLAT_UI_SOUND_BACK };
void plat_request_ui_sound(int sound);
int plat_take_ui_sound(void);

/* Called once the ROM is loaded, before the game starts (the 3DS bottom screen reads its fonts and pictures). */
void plat_rom_loaded(void);

/* Set when the player asks for a snapshot of the next frame (3DS: by touching the bottom screen). */
extern volatile int gPlatSnapshotRequested;
/* The frame number of the last snapshot saved (its files are diff_NNNNN_*), or 0 before the first one. */
extern volatile int gPlatSnapshotSaved;
/* What the player's kart is doing (lap, boost, item, ...), set by the game thread each frame (port_autorun.c) for bug
 * reports to show next to the renderer's frame timing. Kept as raw numbers; only a bug report formats them. */
typedef struct PlatGameNote {
    short racing, lap, checkpoint, bananas, boost, itemType, itemCount, shield, magnet, hit, paused;
} PlatGameNote;
void plat_set_game_note(const PlatGameNote *note);
/* Copies the latest note from another thread. Returns 0 (and leaves out alone) if the game thread was halfway through
 * setting it each time it looked, so a note is never half of one frame and half of the next. */
int plat_get_game_note(PlatGameNote *out);
/* "lap 2 cp 5 bananas 3 boost 0 item 1/3 shield 0 magnet 0 hit 0", or "not racing". */
void plat_format_game_note(const PlatGameNote *note, char *out, int size);

/* Copies the most recent log lines, oldest first, into out (3DS only; empty elsewhere). Returns the length. */
int plat_log_recent(char *out, int size);
/* How many bytes have been logged so far (3DS only; 0 elsewhere). Shows whether logging is still going on. */
unsigned int plat_log_total(void);

/*
 * Shows a block of text lines (separated by '\n') above the watchdog on the bottom screen of the 3DS, replacing the
 * previous block; ignored elsewhere. Used by the profiler.
 */
void plat_overlay_show(const char *text);
/* Sends the unattended test run's results as a GitHub issue, in the background, if bug reports are set up (3DS). */
void plat_send_test_results(const char *title, const char *summary);
/*
 * The 3DS render target: anti-aliasing (the GPU draws more pixels than the screen has and the display transfer averages
 * them; PLAT_AA_4X twice the resolution both ways, PLAT_AA_2X twice the height only). -1 is the player's setting
 * (the Settings menu, else DKR_AA). Colour and depth are always 32-bit: the 16-bit buffers setting was removed
 * (2026-10-29) because it changed the picture, fog among it. The renderer switches between two frames.
 * plat_antialias says what the frames drawn now use (the host build only remembers the setting).
 */
enum { PLAT_AA_OFF, PLAT_AA_4X, PLAT_AA_2X };
void plat_set_gpu_config(int aa);
int plat_antialias(void);
/*
 * Widescreen (roadmap item G3): how much wider than 4:3 the game's camera is to be, 1 for 4:3 or 1.25 to fill the
 * 3DS's 400x240 top screen. Read by the game thread each frame (src/camera.c); the 3DS Settings menu sets it, the host
 * build takes DKR_WIDESCREEN=1.
 */
float plat_aspect_scale(void);
/*
 * The game's vertical field of view (degrees) for the widescreen style: "Wide" keeps it, so the wider picture shows
 * more at the sides and stretches things there more; "Zoomed" narrows it so the sides show what 4:3 did (less at the
 * top and bottom) and nothing is stretched more than on the N64; "Balanced" is half way (in tan terms).
 */
float plat_camera_fov(float fovy);
/* What the style multiplies tan(fovy / 2) by: 1 for Wide, below 1 for the narrower ones (plat_camera_fov). */
float plat_fov_tan_scale(void);
/*
 * The camera's near plane (src/camera.h CAMERA_NEAR): the N64's 10, or 2.5 in widescreen, whose wider picture shows
 * rock faces right beside the camera that 4:3 never reached. Decal pulls and billboards' depth offsets scale with it.
 */
float plat_camera_near(void);
/*
 * Widescreen HUD placement (Settings > Screen > HUD, settings.txt "hudedges", else DKR_HUD_EDGES, on by default): 1
 * moves the race HUD's left and right pieces out to the wide screen's edges (src/game_ui.c), 0 keeps it in the 4:3
 * middle.
 */
int plat_hud_edges(void);
void plat_set_hud_edges(int on);
/*
 * The frame rate cap (roadmap G6): the scheduler hands the game each finished frame after 2 retraces (30 fps, the
 * N64 game's own) or, at 60 fps, after 1. The game's logic already steps by however many retraces a frame took, and
 * falls back to 30 fps by itself when frames don't fit in one. plat_set_60fps is for the GPU benchmark (1 on, 0 off,
 * -1 the player's setting: settings.txt "fps60", else DKR_60FPS); the setting is saved by plat_set_60fps_setting.
 */
int plat_frame_retraces(void);
void plat_set_60fps(int on);
int plat_60fps_setting(void);
void plat_set_60fps_setting(int on);
/* The Frame rate row: 30, 60 or the steady 20 (an Old 3DS's choice; three retraces a frame). */
int plat_frame_rate_setting(void);
void plat_set_frame_rate_setting(int fps);
/* The build's version (git describe), as bug reports show it; "host" for a host build without one. */
const char *plat_build_version(void);
/* The version players see, from the newest version tag (port/Makefile): "0.1.0-beta", "0.1.0-beta+3" or "dev". */
const char *plat_version(void);
/* How the sound is being made, for the test runs' results (e.g. "sys/22k/smooth"; "host" on the host). */
const char *plat_audio_setup(void);
/* The share of the system core an Old 3DS lends the renderer (30, 50 or 80 percent), or 0 when it isn't there. */
int plat_renderer_syscore_share(void);

/*
 * Progress counters, for diagnosing hangs: on the 3DS a watchdog shows them on the bottom screen, so the one that
 * stops counting says which part of the game stopped.
 */
enum PlatCounter {
    PLAT_COUNTER_VI,         /* Retraces seen by the VI thread. */
    PLAT_COUNTER_GFX_QUEUED, /* Display lists handed to the RCP by the game. */
    PLAT_COUNTER_GFX_DONE,   /* Display lists the renderer has finished. */
    PLAT_COUNTER_AUDIO,      /* Audio tasks run. */
    PLAT_COUNTER_MAIN,       /* Main (system) loop iterations. */
    PLAT_COUNTER_AUDIO_UNDERRUN, /* Sound buffers queued after the output had run dry (a gap in the sound). */
    PLAT_COUNTER_AUDIO_DROPPED,  /* Sound buffers dropped because the output was full. */
    PLAT_COUNTER_TEXTURE_FAILED, /* Textures the GPU had no memory for (drawn white instead). */
    PLAT_COUNTER_COUNT
};
extern volatile unsigned int gPlatCounters[PLAT_COUNTER_COUNT];

/*
 * Frames the game may run ahead of the renderer: 1 (the N64's: the game builds a frame while the renderer draws the
 * one before, and waits for it) or 2 (one more, so a slow frame in the renderer is absorbed instead of missed, at the
 * cost of one frame of input lag). Saved as renderahead= in settings.txt; DKR_RENDER_AHEAD in env.txt for a test.
 */
int plat_render_ahead(void);
void plat_set_render_ahead(int frames);
/*
 * The game time the frame the renderer is drawing now covers, in retraces (its logic step), set by the renderer thread
 * before each display list. The Old 3DS frame pacer (gfx_c3d.c) shows each frame that long after the one before.
 */
void plat_set_frame_step(int retraces);
int plat_frame_step(void);
/* The two-core renderer (gfx_split.c) going on or off: the system core's share it needs is held only while on. */
void plat_split_render_cores(int on);

/* The usual setting of the switches from the other ports survey (DKR_GPU_TRANSFORM, DKR_TRACK_MESH, DKR_MODEL_MESH,
 * DKR_STEREO_ONCE) while settings.txt has none: off, until hardware says otherwise; a test build can be made with
 * them all on. Settings > Picture changes them mid-race (debug builds): GPU vertex maths 0 off, 1 on (P30), 2 also
 * the track recorded (P31), 3 also models (P34); 3D made once (P33) 0 or 1. A DKR_* variable wins over both. */
#ifndef GFX_SURVEY_DEFAULT
#define GFX_SURVEY_DEFAULT 0
#endif
int plat_gpu_maths_setting(void);
void plat_set_gpu_maths_setting(int level);
int plat_stereo_once_setting(void);
void plat_set_stereo_once_setting(int on);

/*
 * The first-run setup (port/src/game/port_setup.c). Whether this platform shows it (the 3DS; the PC build only for
 * DKR_SETUP=1); whether the console's own language is French; whether 60 fps can be had (not on an Old 3DS).
 */
int plat_first_run_setup(void);
int plat_system_french(void);
int plat_can_60fps(void);
/* The picture: 0 sharp (30 fps, 4x anti-aliasing), 1 smooth (60 fps, no anti-aliasing). Saved, used from the next
 * frame. */
int plat_picture_style(void);
void plat_set_picture_style(int smooth);
/* The widescreen setting: 0 off (4:3), 1 Wide, 2 Balanced, 3 Zoomed. Saved, used from the next frame. */
int plat_widescreen_style(void);
void plat_set_widescreen_style(int style);
/* What the renderer thread is doing right now. */
extern const char *volatile gPlatGfxStage;

/*
 * The port's own text in the game's language (port/src/game/port_text.c): english as the game writes it now (French
 * when it is set to FRANCAIS), or english itself when there is no translation; and whether it is French now. The
 * game's menus and the 3DS bottom screen use it; a new label, value or hint needs a line in port_text.c.
 */
const char *port_text(const char *english);
int port_text_french(void);

#ifdef __cplusplus
}
#endif

#endif
