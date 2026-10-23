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

typedef struct PlatThread PlatThread;
typedef void (*PlatThreadFunc)(void *arg);

/* Which kind of thread is being created. Used to pick a CPU core and priority band. */
enum PlatThreadKind {
    PLAT_THREAD_GAME,     /* An emulated N64 thread. Priority follows the N64 priority. */
    PLAT_THREAD_HARDWARE, /* Emulated hardware (VI retrace, PI). Always above every game thread. */
    PLAT_THREAD_RCP,      /* Emulated RDP (graphics). May run on a different core. */
    PLAT_THREAD_AUDIO,    /* Emulated RSP audio. May run on a different core. */
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

/* Time the application core has spent idle (only measured on the 3DS; 0 elsewhere). */
unsigned long long plat_idle_ns(void);

/* Monotonic time. */
unsigned long long plat_time_ns(void);
void plat_sleep_ns(unsigned long long ns);

/* The global "kernel" lock and condition variable that protect every emulated message queue. */
void plat_klock(void);
void plat_kunlock(void);
void plat_kwait(void);
/* Waits with a timeout. Returns 0 if woken, 1 on timeout. */
int plat_kwait_timeout(unsigned long long ns);
void plat_kbroadcast(void);

/* A non recursive mutex that emulates "interrupts disabled". Ownership is tracked by the OS layer. */
void plat_intlock_lock(void);
void plat_intlock_unlock(void);

/* Simple counting semaphore used to hand work to the RCP thread. */
typedef struct PlatSem PlatSem;
PlatSem *plat_sem_create(int initial);
void plat_sem_post(PlatSem *sem);
void plat_sem_wait(PlatSem *sem);

/* Waits for the next display refresh (60Hz). */
void plat_wait_vblank(void);

/* Returns nonzero while the application should keep running. Called from the main thread. */
int plat_main_loop_iteration(void);

/* Whole file helpers. plat_load_file returns a malloc'd buffer (or NULL) and writes the size to *size. */
void *plat_load_file(const char *path, unsigned int *size);
int plat_save_file(const char *path, const void *data, unsigned int size);
/* Reads up to size bytes from the start of a file. Returns the number read, or -1 if it cannot be opened. */
int plat_load_file_start(const char *path, void *data, unsigned int size);
void *plat_malloc(size_t size);
void plat_free(void *ptr);
int plat_snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Returns an environment variable / configuration value, or NULL. */
const char *plat_getenv(const char *name);

/* Scripted input (DKR_INPUT) for automated tests. Returns nonzero when a script is set. */
int plat_input_script(int poll, unsigned short *buttons, signed char *stickX, signed char *stickY);

/* Lists the entries of a directory into names (maxNames entries of nameSize bytes). Returns the count. */
int plat_list_dir(const char *dir, char *names, int nameSize, int maxNames);

/* NULL terminated list of directories to search for a ROM with any name. */
const char *const *plat_rom_search_dirs(void);

/* Directory where the ROM and save files live. */
const char *plat_data_dir(void);

void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* A start-up progress line: shown on the bottom screen on the 3DS, and logged. */
void plat_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void plat_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
/* Keeps logging after the start-up lines, which the 3DS otherwise stops at to spare the SD card. */
void plat_log_keep(void);
/* Also writes log lines to this file until called with NULL, which closes it (3DS only). */
void plat_log_copy_to(const char *path);

/*
 * 3DS only: closing the application. The game's threads keep running until the process ends, so the renderer stops
 * at a quit point first (saying whether the GPU was idle), and the process ends without freeing their memory.
 */
enum { PLAT_3DS_PARKED_GPU_IDLE = 1, PLAT_3DS_PARKED_GPU_BUSY = 2 };
void plat_3ds_quit_point(int parkedState);
void plat_3ds_pause_renderer(int pause);
/* Keeps the console awake when the lid closes while held (counted: each hold needs a release). */
void plat_3ds_hold_sleep(int hold);
void plat_3ds_exit(void) __attribute__((noreturn));

/*
 * Pauses the game as if START was pressed (3DS: when the HOME menu opens or the lid closes). The request is set from
 * the system's thread and taken once by the game thread at the start of its next frame.
 */
void plat_request_game_pause(void);
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
 * Anti-aliasing (the 3DS draws at twice the resolution and scales down): 1 on, 0 off, -1 back to the player's setting (the
 * Settings menu, else DKR_AA).
 * The renderer switches between two frames. plat_antialias says whether the frames drawn now are anti-aliased (the
 * host build only remembers the setting).
 */
void plat_set_antialias(int on);
int plat_antialias(void);
/* The build's version (git describe), as bug reports show it; "host" for a host build without one. */
const char *plat_build_version(void);

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
/* What the renderer thread is doing right now. */
extern const char *volatile gPlatGfxStage;

#ifdef __cplusplus
}
#endif

#endif
