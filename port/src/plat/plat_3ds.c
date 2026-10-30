/* Nintendo 3DS (libctru) implementation of the platform layer. */
#include "plat.h"
#include "bottom_screen.h"
#include "settings.h"

#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern u32 __ctru_heap_size;
extern void (*__system_retAddr)(void);

/*
 * All emulated N64 threads run on the application core so that, like on the N64, only one of them executes
 * at any time and a higher priority thread preempts a lower priority one as soon as it becomes ready.
 * 3DS thread priorities run from 0x18 (highest) to 0x3F (lowest), and the main thread normally sits at 0x30.
 */
#define PRIO_HARDWARE 0x19
#define PRIO_RCP 0x1A
/* The sound on the system core: below the system's services there (see plat_init). */
#define PRIO_SYSCORE_AUDIO 0x30
/* The renderer on an Old 3DS's system core: below the system's services there too. */
#define PRIO_SYSCORE_RCP 0x30
#define PRIO_GAME_HIGHEST 0x1B
#define PRIO_GAME_LOWEST 0x3F

struct PlatThread {
    Thread handle;
    PlatThreadFunc func;
    void *arg;
    const char *name;
};

struct PlatSem {
    LightSemaphore sem;
};

static LightLock sKernelLock;
static CondVar sKernelCond;
static CondVar sKeyedConds[PLAT_KEYED_CONDS];
static LightLock sIntLock;
static __thread void *sCurrentOsThread;
static int sRcpCore;
static int sAudioCore;
static int sSyscoreShare; // The system core's share lent to the renderer (percent), when it runs there.
static LightLock sConsoleLock;

static void watchdog_start(void);

static int prio_from_n64(int n64Priority) {
    int prio;
    // N64 priorities: 0 (idle) to 255 (max). The game itself uses 0..150 plus a couple of OS threads at 254/255.
    if (n64Priority >= 150) {
        return PRIO_GAME_HIGHEST;
    }
    // Above the game's main thread (10) are its audio manager (12) and scheduler (13). When the renderer shares their
    // core (an Old 3DS with the renderer on the game's core) it held them up for a whole display list walk (~30 ms
    // there): the sound's small lead over the output ran out, and the audio manager then makes its frames up to 15%
    // longer to catch up, which played the music fast. Both run briefly once a frame, so there they go above the
    // renderer (DKR_SOUND_ABOVE_RENDERER=0 = old).
    if (n64Priority > 10 && sRcpCore == 0 && plat_switch("DKR_SOUND_ABOVE_RENDERER", 1)) {
        return PRIO_HARDWARE;
    }
    prio = PRIO_GAME_LOWEST - n64Priority;
    if (prio < PRIO_GAME_HIGHEST + 1) {
        prio = PRIO_GAME_HIGHEST + 1;
    }
    return prio;
}

void plat_init(void) {
    bool isNew3DS = false;

    LightLock_Init(&sKernelLock);
    CondVar_Init(&sKernelCond);
    for (int i = 0; i < PLAT_KEYED_CONDS; i++) {
        CondVar_Init(&sKeyedConds[i]);
    }
    LightLock_Init(&sIntLock);

    // On a New 3DS the extra cores are free for us, so the emulated RCP gets a core of its own.
    // DKR_SINGLE_CORE=1 keeps everything on the application core, as on an Old 3DS.
    APT_CheckNew3DS(&isNew3DS);
    if (isNew3DS) {
        osSetSpeedupEnable(true);
    }
    sRcpCore = (isNew3DS && plat_getenv("DKR_SINGLE_CORE") == NULL) ? 2 : 0;
    // An Old 3DS has no core of its own to give the renderer, but the system core lends an application a share of
    // its time. The Tools page's "Old 3DS renderer" setting (or DKR_O3DS_RENDER=0-3) moves the renderer there with
    // a share of 30, 50 or 80 percent (roadmap O8): it takes the renderer's ~32 ms off the game's core. The system's
    // own services, the GPU's among them, slow down as the share grows, so it is a bench setting for now.
    if (!isNew3DS && plat_getenv("DKR_SINGLE_CORE") == NULL) {
        static const int sShares[4] = { 0, 30, 50, 80 };
        const char *env = plat_getenv("DKR_O3DS_RENDER");
        int mode = env != NULL ? atoi(env) : settings_get("o3ds_render", 0);
        if (mode > 0 && mode < 4) {
            Result res = APT_SetAppCpuTimeLimit(sShares[mode]);
            plat_log("system core time limit %d%%: %s (0x%08lX)", sShares[mode], R_SUCCEEDED(res) ? "ok" : "failed",
                     (unsigned long) res);
            if (R_SUCCEEDED(res)) {
                sRcpCore = 1;
                sSyscoreShare = sShares[mode];
            }
        }
    }
    plat_status("%s, renderer on core %d", isNew3DS ? "New 3DS" : "Old 3DS", sRcpCore);
    // The scheduler cannot start the next frame while an audio task is running, so audio tasks must finish quickly:
    // they run next to the renderer, at a higher priority than it. The Sound page's "Mixing core" setting can move
    // them to the system core instead (roadmap P9), which only gives an application a share of its time (at the
    // default share they took several times longer), so the share is raised first (DKR_SYSCORE_LIMIT, percent).
    // The system's own services, the GPU's among them, run there too: at the renderer's priority the sound held
    // them up and frame times doubled (bug report #78), so there it runs below them (DKR_SYSCORE_PRIO).
    // With the renderer on the system core the sound stays on the game's core (the DSP voices barely use it).
    // On an Old 3DS it moves the audio task (the DSP voices' event work) at a 50% share by default. The game's audio
    // manager (n64thread4) stays on core 0: the N64 OS emulation (os_core.c) relies on every game thread sharing one
    // core with strict priorities, and the sound lists it edits are shared with the game thread.
    sAudioCore = sRcpCore == 1 ? 0 : sRcpCore;
    if (sRcpCore != 1 && settings_get("sound_core", 0) && plat_getenv("DKR_SINGLE_CORE") == NULL) {
        const char *env = plat_getenv("DKR_SYSCORE_LIMIT");
        int limit = env != NULL ? atoi(env) : isNew3DS ? 80 : 50;
        Result res = APT_SetAppCpuTimeLimit(limit);
        if (R_SUCCEEDED(res)) {
            sAudioCore = 1;
        }
        plat_log("system core time limit %d%%: %s (0x%08lX)", limit, R_SUCCEEDED(res) ? "ok" : "failed",
                 (unsigned long) res);
    }
    plat_status("Sound mixed on core %d", sAudioCore);
    watchdog_start();
    {
        // citro3d is set up (and the GPU self test run) on the main thread, as in SM64's 3DS port.
        extern void gfxb_init(void);
        gfxb_init();
    }
}

/*
 * With DKR_PROFILE set, a lowest priority thread on the application core measures how much time is left over.
 * It spins in short slices and counts how long each slice took. On a New 3DS a second one does the same on the
 * renderer's core: the game core is nearly always idle there, since the renderer and the sound run elsewhere.
 */
static volatile unsigned long long sIdleNs, sRenderIdleNs;

static void idle_meter(void *arg) {
    volatile unsigned long long *counter = arg;
    for (;;) {
        unsigned long long start = plat_time_ns(), now;
        do {
            now = plat_time_ns();
        } while (now - start < 200000ULL);
        // A slice that took much longer than 0.2ms means higher priority threads ran in between.
        *counter += 200000ULL;
    }
}

static void start_idle_meters(void) {
    static int sStarted;
    if (!sStarted) {
        sStarted = 1;
        threadCreate(idle_meter, (void *) &sIdleNs, 4096, 0x3F, 0, true);
        // Not on the system core: spinning there would use up the time it lends the renderer.
        if (sRcpCore > 1) {
            threadCreate(idle_meter, (void *) &sRenderIdleNs, 4096, 0x3F, sRcpCore, true);
        }
    }
}

int plat_renderer_syscore_share(void) {
    return sRcpCore == 1 ? sSyscoreShare : 0;
}

/*
 * The two-core renderer going on or off (gfx_split.c). With the renderer on the game's core its second half runs on
 * the system core, which needs a share of that core (DKR_SPLIT_SHARE, 80% by default): raised while it is on, and put
 * back as it was when it goes off, so the switch bench's other races run with the system's services as usual.
 */
void plat_split_render_cores(int on) {
    static int sRaised;
    static u32 sBefore;
    if (sRcpCore != 0) {
        return;
    }
    if (on && !sRaised) {
        const char *env = plat_getenv("DKR_SPLIT_SHARE");
        int share = env != NULL ? atoi(env) : 80;
        Result res;
        if (R_FAILED(APT_GetAppCpuTimeLimit(&sBefore))) {
            sBefore = 0;
        }
        res = APT_SetAppCpuTimeLimit(share);
        plat_log("system core time limit %d%% for the two-core renderer (was %lu%%): %s (0x%08lX)", share,
                 (unsigned long) sBefore, R_SUCCEEDED(res) ? "ok" : "failed", (unsigned long) res);
        if (R_SUCCEEDED(res)) {
            sRaised = 1;
            sSyscoreShare = share;
        }
    } else if (!on && sRaised) {
        Result res = APT_SetAppCpuTimeLimit(sBefore >= 5 ? sBefore : 5);
        plat_log("system core time limit back to %lu%%: %s (0x%08lX)", (unsigned long) (sBefore >= 5 ? sBefore : 5),
                 R_SUCCEEDED(res) ? "ok" : "failed", (unsigned long) res);
        sRaised = 0;
        sSyscoreShare = 0;
    }
}

/* The Sound page's results label (audio_ndsp.c). */
int plat_3ds_audio_on_system_core(void) {
    return sAudioCore == 1;
}

unsigned long long plat_idle_ns(void) {
    start_idle_meters();
    return sIdleNs;
}

unsigned long long plat_render_idle_ns(void) {
    start_idle_meters();
    return sRcpCore > 1 ? sRenderIdleNs : sIdleNs;
}

void plat_shutdown(void) {
}

/* In bugreport_3ds.c: writes crash.txt if the thread crashes. */
void bugreport_watch_thread(const char *name);

static volatile int sQuitting;
static volatile int sRendererParked;
static volatile int sPauseRequested;
static volatile int sRendererPaused;

/*
 * Called by the renderer between frames (with the GPU idle) and while it waits for the GPU. Holds it while
 * the application is suspended (see plat_3ds_pause_renderer), and parks it for good once the application is closing
 * (see plat_3ds_exit).
 */
void plat_3ds_quit_point(int parkedState) {
    if (parkedState == PLAT_3DS_PARKED_GPU_IDLE && sPauseRequested && !sQuitting) {
        // A renderer (or half of one) on an Old 3DS's system core gives its share of that core back while the game is away: the share
        // holds even while the renderer waits, and slowed the HOME menu, whose services run there.
        int lent = sSyscoreShare > 0; // The renderer's, or the two-core renderer's second half's
        if (lent) {
            Result res = APT_SetAppCpuTimeLimit(5);
            plat_log("system core time limit 5%% while away: %s (0x%08lX)", R_SUCCEEDED(res) ? "ok" : "failed",
                     (unsigned long) res);
        }
        while (sPauseRequested && !sQuitting) {
            sRendererPaused = 1;
            svcSleepThread(1000000);
        }
        sRendererPaused = 0;
        if (lent && !sQuitting) {
            Result res = APT_SetAppCpuTimeLimit(sSyscoreShare);
            plat_log("system core time limit %d%% again: %s (0x%08lX)", sSyscoreShare, R_SUCCEEDED(res) ? "ok" : "failed",
                     (unsigned long) res);
        }
    }
    if (!sQuitting) {
        return;
    }
    sRendererParked = parkedState;
    for (;;) {
        svcSleepThread(1000000000LL);
    }
}

int plat_3ds_renderer_should_park(void) {
    return sPauseRequested || sQuitting;
}

/*
 * The HOME menu takes the GPU away while the application is suspended. The renderer kept drawing frames meanwhile,
 * and the GPU hung when the game was resumed. So it waits at the start of a frame, with the GPU idle, until the
 * application is restored.
 */
void plat_3ds_pause_renderer(int pause) {
    unsigned long long start = plat_time_ns();
    sPauseRequested = pause;
    if (!pause) {
        return;
    }
    while (!sRendererPaused && plat_time_ns() - start < 2000000000ULL) {
        svcSleepThread(1000000);
    }
}

void port_audio_output_exit(void);
void gfxb_3ds_shutdown(void);

/*
 * Ends the application. Returning from main would unmap the heap while the game's threads still run on it (their
 * stacks live there too), which crashed the whole system on exit. Instead the renderer is stopped, the sound and the
 * GPU are shut down, APT is told the application has closed, and the process ends, which stops every thread at once.
 */
void plat_3ds_exit(void) {
    unsigned long long start = plat_time_ns();

    sQuitting = 1;
    while (sRendererParked == 0 && plat_time_ns() - start < 1000000000ULL) {
        svcSleepThread(1000000);
    }
    port_audio_output_exit();
    // Nothing may use the bottom screen console once the screens are gone: hold its lock for good.
    LightLock_Lock(&sConsoleLock);
    if (sRendererParked == PLAT_3DS_PARKED_GPU_IDLE) {
        gfxb_3ds_shutdown();
    }
    if (sRendererParked != 0) {
        gfxExit();
    }
    {
        // When the system closed the application, aptExit leaves the last step, telling the HOME menu the
        // application has closed, to the exit code (which would run it after freeing the heap). Without it the HOME
        // menu crashed when it next launched something. It also ends the process.
        void (*before)(void) = __system_retAddr;
        aptExit();
        if (__system_retAddr != before && __system_retAddr != NULL) {
            __system_retAddr();
        }
    }
    svcExitProcess();
}

static void thread_entry(void *arg) {
    PlatThread *thread = (PlatThread *) arg;
    bugreport_watch_thread(thread->name);
    thread->func(thread->arg);
}

PlatThread *plat_thread_create(PlatThreadFunc func, void *arg, int n64Priority, int kind, const char *name) {
    PlatThread *thread = calloc(1, sizeof(PlatThread));
    int prio, core;
    size_t stackSize = 64 * 1024;

    thread->func = func;
    thread->arg = arg;
    thread->name = name;
    switch (kind) {
        case PLAT_THREAD_HARDWARE:
            prio = PRIO_HARDWARE;
            core = 0;
            stackSize = 16 * 1024;
            break;
        case PLAT_THREAD_RCP:
            prio = PRIO_RCP;
            core = sRcpCore;
            if (core == 1) {
                const char *env = plat_getenv("DKR_SYSCORE_RCP_PRIO");
                prio = env != NULL ? (int) strtol(env, NULL, 0) : PRIO_SYSCORE_RCP;
            }
            stackSize = 128 * 1024;
            break;
        case PLAT_THREAD_AUDIO:
            // Above the renderer: 3DS threads of equal priority never preempt each other, so an audio task that
            // arrives while a frame is being drawn would otherwise wait for the whole frame and the sound would
            // run dry.
            prio = PRIO_HARDWARE;
            core = sAudioCore;
            if (core == 1) {
                const char *env = plat_getenv("DKR_SYSCORE_PRIO");
                prio = env != NULL ? (int) strtol(env, NULL, 0) : PRIO_SYSCORE_AUDIO;
            }
            stackSize = 64 * 1024;
            break;
        case PLAT_THREAD_RCP2:
            // The other core from the walk's: the system core when the renderer is on the game's (its share raised
            // to DKR_SPLIT_SHARE, 80% by default, unless the sound already holds one), else the game's core, where
            // it goes above the game as the renderer does.
            stackSize = 64 * 1024;
            if (sRcpCore == 1) {
                core = 0;
                prio = PRIO_RCP;
            } else {
                // The share was raised first (plat_split_render_cores).
                core = 1;
                // Below the audio task when that is on the system core too (Mixing core System): threads of equal
                // priority never preempt each other, and a backend behind by a frame would hold the sound up.
                prio = PRIO_SYSCORE_AUDIO + 1;
            }
            break;
        default:
            prio = prio_from_n64(n64Priority);
            core = 0;
            break;
    }
    thread->handle = threadCreate(thread_entry, thread, stackSize, prio, core, true);
    if (thread->handle == NULL && kind == PLAT_THREAD_AUDIO && core != sRcpCore) {
        plat_log("thread %s: core %d unavailable, using the renderer's core %d", name, core, sRcpCore);
        core = sAudioCore = sRcpCore;
        thread->handle = threadCreate(thread_entry, thread, stackSize, prio, core, true);
    }
    if (thread->handle == NULL && core != 0) {
        plat_log("thread %s: core %d unavailable, using core 0", name, core);
        if (kind == PLAT_THREAD_RCP) {
            sRcpCore = 0;
            sSyscoreShare = 0;
            prio = PRIO_RCP;
        }
        if (kind == PLAT_THREAD_RCP2) {
            prio = PRIO_RCP;
        }
        core = 0;
        thread->handle = threadCreate(thread_entry, thread, stackSize, prio, 0, true);
    }
    plat_log("thread %s: priority 0x%X, core %d", name, prio, core);
    if (thread->handle == NULL) {
        plat_fatal("Could not create thread %s", name);
    }
    return thread;
}

void plat_thread_set_priority(PlatThread *thread, int n64Priority) {
    Handle handle = (thread != NULL) ? threadGetHandle(thread->handle) : CUR_THREAD_HANDLE;
    svcSetThreadPriority(handle, prio_from_n64(n64Priority));
}

void plat_thread_park(void) {
    for (;;) {
        svcSleepThread(1000000000LL);
    }
}

void plat_set_current_os_thread(void *thread) {
    sCurrentOsThread = thread;
}

void *plat_get_current_os_thread(void) {
    return sCurrentOsThread;
}

unsigned long long plat_time_ns(void) {
    // The system tick runs at SYSCLOCK_ARM11 (268MHz).
    unsigned long long ticks = svcGetSystemTick();
    return (ticks / SYSCLOCK_ARM11) * 1000000000ULL + ((ticks % SYSCLOCK_ARM11) * 1000000000ULL) / SYSCLOCK_ARM11;
}

unsigned long long plat_ticks(void) {
    return svcGetSystemTick();
}

unsigned long long plat_ticks_per_second(void) {
    return SYSCLOCK_ARM11;
}

void plat_sleep_ns(unsigned long long ns) {
    svcSleepThread((s64) ns);
}

void plat_klock(void) {
    LightLock_Lock(&sKernelLock);
}

void plat_kunlock(void) {
    LightLock_Unlock(&sKernelLock);
}

/*
 * Bounded, as every caller loops on its condition: a broadcast the condition variable loses then costs a few
 * milliseconds instead of a frozen game (see QUEUE_WAIT_NS in os_core.c).
 */
void plat_kwait(void) {
    CondVar_WaitTimeout(&sKernelCond, &sKernelLock, 4000000LL);
}

int plat_kwait_timeout(unsigned long long ns) {
    return CondVar_WaitTimeout(&sKernelCond, &sKernelLock, (s64) ns) != 0;
}

void plat_kbroadcast(void) {
    CondVar_Broadcast(&sKernelCond);
}

int plat_kwait_on_timeout(const void *key, unsigned long long ns) {
    return CondVar_WaitTimeout(&sKeyedConds[PLAT_KEY_SLOT(key)], &sKernelLock, (s64) ns) != 0;
}

void plat_kbroadcast_on(const void *key) {
    CondVar_Broadcast(&sKeyedConds[PLAT_KEY_SLOT(key)]);
}

void plat_intlock_lock(void) {
    LightLock_Lock(&sIntLock);
}

void plat_intlock_unlock(void) {
    LightLock_Unlock(&sIntLock);
}

PlatSem *plat_sem_create(int initial) {
    PlatSem *sem = calloc(1, sizeof(PlatSem));
    LightSemaphore_Init(&sem->sem, initial, 0x7FFF);
    return sem;
}

void plat_sem_post(PlatSem *sem) {
    LightSemaphore_Release(&sem->sem, 1);
}

void plat_sem_wait(PlatSem *sem) {
    LightSemaphore_Acquire(&sem->sem, 1);
}

int plat_sem_try_wait(PlatSem *sem) {
    return LightSemaphore_TryAcquire(&sem->sem, 1) == 0;
}

void plat_wait_vblank(void) {
    gspWaitForVBlank();
}

const char *const *plat_rom_search_dirs(void) {
    static const char *const sDirs[] = { "sdmc:/3ds/dkr", "sdmc:/3ds", "sdmc:", NULL };
    return sDirs;
}

const char *plat_data_dir(void) {
    return "sdmc:/3ds/dkr";
}

/*
 * The start-up is always logged to sdmc:/3ds/dkr/log.txt (the first LOG_BOOT_LINES lines), so there is something to
 * look at if the game does not start. Creating sdmc:/3ds/dkr/debug keeps logging after that.
 */
#define LOG_BOOT_LINES 200

static LightLock sLogLock;
static int sLogLines;
static int sLogDebug;
static FILE *sLogCopy;

/* Every line also goes into a ring buffer, for bug reports: the file stops after the start-up lines. */
#define LOG_RING_SIZE (48 * 1024)
static char sLogRing[LOG_RING_SIZE];
static unsigned int sLogRingEnd; // Total bytes ever written; the ring holds the last LOG_RING_SIZE of them.

static void ring_add(const char *text, int length) {
    int i;
    for (i = 0; i < length; i++) {
        sLogRing[(sLogRingEnd + i) % LOG_RING_SIZE] = text[i];
    }
    sLogRingEnd += length;
}

unsigned int plat_log_total(void) {
    return sLogRingEnd;
}

/* Also used by the crash handler, without the lock: it must not wait for a thread that may have crashed holding it. */
int plat_log_recent(char *out, int size) {
    unsigned int end = sLogRingEnd, start, i;
    unsigned int available = (end < LOG_RING_SIZE) ? end : LOG_RING_SIZE - 256;
    if (size <= 0) {
        return 0;
    }
    if (available > (unsigned int) size - 1) {
        available = size - 1;
    }
    start = end - available;
    for (i = 0; i < available; i++) {
        out[i] = sLogRing[(start + i) % LOG_RING_SIZE];
    }
    out[available] = '\0';
    // Start at a whole line.
    if (start > 0) {
        char *newline = strchr(out, '\n');
        if (newline != NULL) {
            memmove(out, newline + 1, strlen(newline + 1) + 1);
        }
    }
    return strlen(out);
}

static FILE *log_file(void) {
    static int sChecked;
    static FILE *sFile;
    if (!sChecked) {
        FILE *flag = fopen("sdmc:/3ds/dkr/debug", "rb");
        sChecked = 1;
        if (flag != NULL) {
            fclose(flag);
            sLogDebug = 1;
        }
        sFile = fopen("sdmc:/3ds/dkr/log.txt", "w");
    }
    if (!sLogDebug && sLogLines >= LOG_BOOT_LINES) {
        if (sFile != NULL && sLogLines == LOG_BOOT_LINES) {
            // Said in the file, so that its end is not taken for the moment the game stopped.
            fprintf(sFile, "(log.txt ends here; bug reports carry the latest lines, and a hang saves them to "
                           "hang_log.txt)\n");
            fflush(sFile);
            sLogLines++;
        }
        return NULL;
    }
    return sFile;
}

static void log_line(const char *prefix, const char *text) {
    static int sLockReady;
    FILE *file;
    if (!sLockReady) {
        // The first line is logged from the main thread before any other thread exists.
        LightLock_Init(&sLogLock);
        sLockReady = 1;
    }
    LightLock_Lock(&sLogLock);
    {
        char line[600];
        int length = snprintf(line, sizeof(line), "%7llu %s%s\n", plat_time_ns() / 1000000ULL, prefix, text);
        ring_add(line, length < (int) sizeof(line) ? length : (int) sizeof(line) - 1);
    }
    file = log_file();
    if (file != NULL) {
        fprintf(file, "%7llu %s%s\n", plat_time_ns() / 1000000ULL, prefix, text);
        fflush(file);
        sLogLines++;
    }
    if (sLogCopy != NULL) {
        fprintf(sLogCopy, "%7llu %s%s\n", plat_time_ns() / 1000000ULL, prefix, text);
    }
    LightLock_Unlock(&sLogLock);
}

void plat_log_keep(void) {
    sLogDebug = 1;
}

/* Snapshots get a log file of their own: some ways of copying files off the SD card kept serving an old log.txt. */
extern volatile u32 gPortAudioEventsDropped, gPortAudioQueueEmpty;

void plat_log_copy_to(const char *path) {
    FILE *file = (path != NULL) ? fopen(path, "w") : NULL;
    FILE *old;
    LightLock_Lock(&sLogLock);
    old = sLogCopy;
    sLogCopy = file;
    LightLock_Unlock(&sLogLock);
    if (old != NULL) {
        fclose(old);
    }
    if (file != NULL) {
        struct mallinfo info = mallinfo();
        plat_log("free memory: gpu %lu KB, heap %lu KB; texture failures %u, sound gaps %u, drops %u, sound events "
                 "dropped %lu, empty sound queues %lu",
                 (unsigned long) (linearSpaceFree() / 1024),
                 (unsigned long) ((info.fordblks + (__ctru_heap_size - info.arena)) / 1024),
                 gPlatCounters[PLAT_COUNTER_TEXTURE_FAILED], gPlatCounters[PLAT_COUNTER_AUDIO_UNDERRUN],
                 gPlatCounters[PLAT_COUNTER_AUDIO_DROPPED], (unsigned long) gPortAudioEventsDropped,
                 (unsigned long) gPortAudioQueueEmpty);
    }
}

void plat_log(const char *fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    svcOutputDebugString(buffer, strlen(buffer));
    log_line("", buffer);
#ifdef PORT_DEBUG_CONSOLE
    printf("%s\n", buffer);
#endif
}

/*
 * The bottom screen (bottom_screen.c draws it): the logo, the controls and the newest status messages, or the Settings
 * menu (menu_3ds.c) while it is open; under them the buttons (update_3ds.c) and the watchdog's two lines. The calls
 * here change what it shows; a thread of its own draws the changed areas into sImage and copies them to the screen,
 * so the game's threads never wait for the drawing. Until that thread starts (plat_rom_loaded), the caller draws.
 */
static u16 sImage[BS_WIDTH * BS_HEIGHT];
static LightEvent sScreenEvent;
static volatile int sScreenThreadStarted;
static volatile unsigned long long sScreenBusyNs; // Time its thread has spent drawing (the profiler's P36 line)
static volatile unsigned long sScreenDraws;       // Areas it has drawn

int plat_old_3ds(void) {
    static int sOld = -1;
    if (sOld < 0) {
        bool isNew3DS = false;
        APT_CheckNew3DS(&isNew3DS);
        sOld = !isNew3DS;
    }
    return sOld;
}

unsigned long long plat_screen_busy_ns(void) {
    return sScreenBusyNs;
}

unsigned long plat_screen_draws(void) {
    return sScreenDraws;
}

/* Copies rows of the image to the framebuffer, which is the screen turned on its side (the console lock is held). */
static void screen_copy(int first, int end) {
    u16 width, height;
    u16 *fb = (u16 *) gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &width, &height);
    int x, y;
    if ((u32) fb >= OS_VRAM_VADDR && (u32) fb < OS_VRAM_VADDR + OS_VRAM_SIZE) {
        return; // DKR_VRAM_SCREEN=1 (a test): the CPU may not write VRAM, so the bottom screen stays blank.
    }
    for (x = 0; x < BS_WIDTH; x++) {
        u16 *column = fb + x * 240 + 239;
        for (y = first; y < end; y++) {
            column[-y] = sImage[y * BS_WIDTH + x];
        }
    }
    // Nothing else flushes the bottom screen's framebuffer from the CPU cache.
    GSPGPU_FlushDataCache(fb, width * height * 2);
}

/* Draws what changed. Only one thread at a time: the screen's own, or before it starts the caller (lock held). */
static void screen_draw(unsigned changes) {
    int area;
    for (area = 0; area < BS_AREA_COUNT; area++) {
        if (changes & (1 << area)) {
            int first, end;
            bs_draw(sImage, area);
            bs_area_rows(area, &first, &end);
            screen_copy(first, end);
        }
    }
}

/* After a change (the console lock is held). */
static void screen_changed(void) {
    if (sScreenThreadStarted) {
        LightEvent_Signal(&sScreenEvent);
    } else {
        screen_draw(bs_take_changes());
    }
}

static void screen_thread(void *arg) {
    (void) arg;
    bugreport_watch_thread("screen");
    for (;;) {
        unsigned changes;
        LightEvent_Wait(&sScreenEvent);
        LightLock_Lock(&sConsoleLock);
        changes = bs_take_changes();
        LightLock_Unlock(&sConsoleLock);
        // Drawn without the lock, from the copy taken; only the copy to the screen holds it (see plat_3ds_exit).
        if (changes != 0) {
            unsigned long long start = plat_time_ns();
            int area;
            for (area = 0; area < BS_AREA_COUNT; area++) {
                if (changes & (1 << area)) {
                    int first, end;
                    bs_draw(sImage, area);
                    bs_area_rows(area, &first, &end);
                    LightLock_Lock(&sConsoleLock);
                    screen_copy(first, end);
                    LightLock_Unlock(&sConsoleLock);
                    sScreenDraws++;
                }
            }
            // Includes any time other threads took the core meanwhile: on the game's core at a low priority, this
            // is an upper bound.
            sScreenBusyNs += plat_time_ns() - start;
        }
    }
}

/*
 * The drawing thread runs at a low priority next to the renderer (on its core on a New 3DS, which waits on the GPU
 * much of the time; on the game's core when an Old 3DS lends the renderer the system core's time), so the screen is
 * only drawn when nothing else needs that core.
 */
static void screen_thread_start(int core) {
    LightLock_Lock(&sConsoleLock);
    LightEvent_Init(&sScreenEvent, RESET_ONESHOT);
    if (threadCreate(screen_thread, NULL, 16 * 1024, 0x3E, core, true) != NULL) {
        sScreenThreadStarted = 1;
        LightEvent_Signal(&sScreenEvent);
    } else {
        plat_log("bottom screen: could not create its thread");
    }
    LightLock_Unlock(&sConsoleLock);
}

/* First thing in main, before plat_init. */
void plat_3ds_console_init(void) {
    LightLock_Init(&sConsoleLock);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    gfxSwapBuffersGpu();
    gspWaitForVBlank();
    LightLock_Lock(&sConsoleLock);
    bs_init(consoleGetDefault()->font.gfx);
    screen_changed();
    LightLock_Unlock(&sConsoleLock);
}

/*
 * The ROM is loaded (port_boot, before the game's threads start): the screen gets the game's fonts and pictures, and
 * from now on its own thread draws it.
 */
void plat_rom_loaded(void) {
    LightLock_Lock(&sConsoleLock);
    bs_load_assets();
    screen_changed();
    LightLock_Unlock(&sConsoleLock);
    screen_thread_start(sRcpCore == 1 ? 0 : sRcpCore);
}

/* The buttons (see bs_set_buttons). */
void plat_3ds_set_buttons(const char *const *labels, int count) {
    LightLock_Lock(&sConsoleLock);
    bs_set_buttons(labels, count);
    screen_changed();
    LightLock_Unlock(&sConsoleLock);
}

/* A line of text in place of the buttons. */
void plat_3ds_panel_text(const char *text) {
    LightLock_Lock(&sConsoleLock);
    bs_set_panel_text(text);
    screen_changed();
    LightLock_Unlock(&sConsoleLock);
}

/* The Settings menu's page, or NULL when it closes. */
void plat_3ds_menu_draw(const BsMenu *menu) {
    LightLock_Lock(&sConsoleLock);
    bs_show_menu(menu);
    screen_changed();
    LightLock_Unlock(&sConsoleLock);
}

void update_3ds_race_changed(int racing);

/*
 * The race HUD (race_hud.h), from the game thread once a frame, or NULL after a race (a menu's companion between
 * races). The screen's thread draws the newest one when it gets to it; the panel's buttons change (Pause joins
 * Settings) when a race starts or ends.
 */
int plat_race_hud_screen(void) {
    return 1;
}

/* Whether the bottom screen shows something over the race HUD (the Settings menu, the profiler's lines). */
int plat_race_hud_covered(void) {
    return bs_race_hud_covered();
}

/* Whether a HUD only differs from the last one drawn in what moves every frame (the timer and the map). */
static int same_standing(const RaceHud *a, const RaceHud *b) {
    int i;
    if (a->mode != b->mode || a->position != b->position || a->lap != b->lap || a->item != b->item ||
        a->itemLevel != b->itemLevel || a->itemCount != b->itemCount || a->bananas != b->bananas ||
        a->finished != b->finished || a->silverCoins != b->silverCoins || a->racersLeft != b->racersLeft ||
        a->balloons != b->balloons || a->racerCount != b->racerCount) {
        return 0;
    }
    for (i = 0; i < a->racerCount && i < RACE_HUD_RACERS; i++) {
        if (a->racers[i].score != b->racers[i].score || a->racers[i].carrying != b->racers[i].carrying ||
            a->racers[i].out != b->racers[i].out) {
            return 0;
        }
    }
    return 1;
}

void plat_set_race_hud(const RaceHud *hud) {
    static int sShown, sRacing, sEpoch = -1;
    static u64 sIntervalNs = 66000000ULL;
    static RaceHud sLast;
    static u64 sLastNs;
    u64 now = svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000000) * 1000;
    int racing = hud != NULL && hud->mode < RACE_HUD_MODE_FIRST_MENU;
    if (sEpoch != gPlatSwitchEpoch) {
        // DKR_BS_HUD_MS (P36, a switch bench entry): the race HUD's redraw interval. On an Old 3DS the screen's thread
        // shares the game's core.
        int ms = plat_switch("DKR_BS_HUD_MS", 66);
        sEpoch = gPlatSwitchEpoch;
        sIntervalNs = (u64) (ms > 0 ? ms : 66) * 1000000ULL;
    }
    if (hud != NULL && sShown) {
        // A menu's companion only changes with what the menu has highlighted. In a race, the timer and the map move
        // every frame; 15 redraws a second is plenty for them, which keeps the screen's thread from taking much of
        // its core. A change of place, lap, item, bananas or scores is drawn straight away.
        if (!racing ? memcmp(hud, &sLast, sizeof(*hud)) == 0
                    : now - sLastNs < sIntervalNs && same_standing(hud, &sLast)) {
            return;
        }
    }
    sShown = hud != NULL;
    if (hud != NULL) {
        sLast = *hud;
        sLastNs = now;
    }
    LightLock_Lock(&sConsoleLock);
    bs_set_race_hud(hud);
    screen_changed();
    LightLock_Unlock(&sConsoleLock);
    if (racing != sRacing) {
        sRacing = racing;
        update_3ds_race_changed(sRacing);
    }
}

/* The profiler's readout, under the messages. */
void plat_overlay_show(const char *text) {
    LightLock_Lock(&sConsoleLock);
    bs_set_overlay(text);
    screen_changed();
    LightLock_Unlock(&sConsoleLock);
}

/* A line among the status messages that is not logged (the bug report's question at start). */
void plat_3ds_console_line(const char *text) {
    LightLock_Lock(&sConsoleLock);
    bs_add_message(text);
    screen_changed();
    LightLock_Unlock(&sConsoleLock);
}

void plat_3ds_console_clear(void) {
    LightLock_Lock(&sConsoleLock);
    bs_clear_messages();
    screen_changed();
    LightLock_Unlock(&sConsoleLock);
}

void plat_status(const char *fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    log_line("", buffer);
    plat_3ds_console_line(buffer);
}

static void status_lines(const char *first, const char *second) {
    LightLock_Lock(&sConsoleLock);
    bs_set_status_line(0, first);
    bs_set_status_line(1, second);
    screen_changed();
    LightLock_Unlock(&sConsoleLock);
}

/*
 * The watchdog shows the progress counters (plat.h) at the bottom of the bottom screen once a second, and logs them
 * every few seconds for the first minute. Its thread has the highest priority an application can use, so it keeps
 * running even if a game thread never yields; if its line stops updating, the whole system is stuck (for example
 * the GPU).
 */
long port_autorun_enabled(void);
void port_describe_game_state(char *buffer, long size);
extern volatile unsigned long gPortEventsDropped;
extern volatile unsigned long gPortEventsWaiting;
void port_describe_sched(char *buffer, long size);
void port_describe_threads(char *buffer, long size);
void port_describe_rcp(char *buffer, long size);

/*
 * What every part of the emulated N64 is doing, logged 3 seconds into a stall (so a bug report sent soon after has it)
 * and again with the hang log: the game's stage, the scheduler, the RCP workers, the queue each N64 thread is blocked
 * on, and how busy the cores were over the last second (busy cores with nothing moving mean a thread is spinning;
 * idle ones mean the threads are waiting on each other).
 */
static void log_stall_state(int seconds, unsigned idlePct, unsigned renderIdlePct) {
    char text[512];
    snprintf(text, sizeof(text), "stalled %d s: game core idle %u%%, render core idle %u%%, %lu done messages waiting, "
             "%lu event messages lost, %lu sound events dropped, %lu empty sound queues\n", seconds, idlePct,
             renderIdlePct, gPortEventsWaiting, gPortEventsDropped, (unsigned long) gPortAudioEventsDropped,
             (unsigned long) gPortAudioQueueEmpty);
    log_line("watchdog: ", text);
    port_describe_game_state(text, sizeof(text));
    log_line("watchdog: ", text);
    port_describe_sched(text, sizeof(text));
    log_line("watchdog: ", text);
    port_describe_rcp(text, sizeof(text));
    log_line("watchdog: ", text);
    port_describe_threads(text, sizeof(text));
    log_line("watchdog: ", text);
}

/*
 * A renderer that has drawn nothing for 20 seconds (outside the HOME menu) is taken as a hang: the recent log is saved
 * to hang_log.txt, which the bug report on the next launch sends, and during the unattended test run the hang is
 * written down for the next launch to record (port_autorun.c). Both files go again if drawing resumes.
 */
static void check_for_hang(int framesDrawn, int *stalledSeconds, int *written, unsigned idlePct,
                           unsigned renderIdlePct) {
    static char sRecent[40 * 1024];
    char path[256], logPath[256], text[160];
    snprintf(path, sizeof(path), "%s/autorun_hang.txt", plat_data_dir());
    snprintf(logPath, sizeof(logPath), "%s/hang_log.txt", plat_data_dir());
    if (framesDrawn || sPauseRequested) {
        *stalledSeconds = 0;
        if (*written) {
            *written = 0;
            remove(path);
            remove(logPath);
        }
        return;
    }
    // Idle meters are not started here: they spin at the idle thread's priority and would starve it during start-up.
    if (++*stalledSeconds == 3) {
        log_stall_state(*stalledSeconds, idlePct, renderIdlePct);
    }
    if (*stalledSeconds == 20) {
        snprintf(text, sizeof(text), "renderer stuck while %s (gfx %u/%u, vi %u)\n",
                 gPlatGfxStage != NULL ? gPlatGfxStage : "-", gPlatCounters[PLAT_COUNTER_GFX_QUEUED],
                 gPlatCounters[PLAT_COUNTER_GFX_DONE], gPlatCounters[PLAT_COUNTER_VI]);
        log_line("watchdog: ", text);
        log_stall_state(*stalledSeconds, idlePct, renderIdlePct);
        *written = 1;
        if (port_autorun_enabled()) {
            plat_save_file(path, text, strlen(text));
        }
        plat_save_file(logPath, sRecent, plat_log_recent(sRecent, sizeof(sRecent)));
    }
}

static void watchdog(void *arg) {
    unsigned int last[PLAT_COUNTER_COUNT];
    int seconds = 0, stalledSeconds = 0, hangWritten = 0;
    unsigned long long lastIdle = 0, lastRenderIdle = 0;

    (void) arg;
    memset(last, 0, sizeof(last));
    for (;;) {
        char line[2][64];
        unsigned int now[PLAT_COUNTER_COUNT];
        int i;

        svcSleepThread(1000000000LL);
        seconds++;
        for (i = 0; i < PLAT_COUNTER_COUNT; i++) {
            now[i] = gPlatCounters[i];
        }
        snprintf(line[0], sizeof(line[0]), "%3ds vi%+4d gfx %u/%u aud%+4d sys%+3d", seconds,
                 (int) (now[PLAT_COUNTER_VI] - last[PLAT_COUNTER_VI]), now[PLAT_COUNTER_GFX_QUEUED],
                 now[PLAT_COUNTER_GFX_DONE], (int) (now[PLAT_COUNTER_AUDIO] - last[PLAT_COUNTER_AUDIO]),
                 (int) (now[PLAT_COUNTER_MAIN] - last[PLAT_COUNTER_MAIN]));
        // Sound gaps and drops, textures without memory, free GPU (linear) and CPU (heap) memory in tenths of a MB.
        {
            struct mallinfo info = mallinfo();
            unsigned long gpuFree = linearSpaceFree() / (1024 * 1024 / 10);
            unsigned long heapFree = (info.fordblks + (__ctru_heap_size - info.arena)) / (1024 * 1024 / 10);
            snprintf(line[1], sizeof(line[1]), "gap %u drop %u tex %u gpu %lu.%luM heap %lu.%luM",
                     now[PLAT_COUNTER_AUDIO_UNDERRUN], now[PLAT_COUNTER_AUDIO_DROPPED],
                     now[PLAT_COUNTER_TEXTURE_FAILED], gpuFree / 10, gpuFree % 10, heapFree / 10, heapFree % 10);
        }
        {
            // A full sound event queue loses events (and, before the fix in csplayer.c, froze the game).
            static u32 sLoggedDropped, sLoggedEmpty;
            u32 dropped = gPortAudioEventsDropped, empty = gPortAudioQueueEmpty;
            if (dropped != sLoggedDropped || empty != sLoggedEmpty) {
                char text[96];
                snprintf(text, sizeof(text), "sound events dropped %lu, empty sound queues %lu\n",
                         (unsigned long) dropped, (unsigned long) empty);
                log_line("watchdog: ", text);
                sLoggedDropped = dropped;
                sLoggedEmpty = empty;
            }
        }
        {
            // Idle time is only counted while the profiler runs (a test run turns it on); 0 otherwise.
            unsigned long long idle = sIdleNs, renderIdle = sRcpCore > 1 ? sRenderIdleNs : sIdleNs;
            unsigned idlePct = lastIdle != 0 ? (unsigned) ((idle - lastIdle) / 10000000ULL) : 0;
            unsigned renderIdlePct = lastRenderIdle != 0 ? (unsigned) ((renderIdle - lastRenderIdle) / 10000000ULL) : 0;
            lastIdle = idle;
            lastRenderIdle = renderIdle;
            check_for_hang(now[PLAT_COUNTER_GFX_DONE] != last[PLAT_COUNTER_GFX_DONE], &stalledSeconds, &hangWritten,
                           idlePct, renderIdlePct);
        }
        memcpy(last, now, sizeof(last));

        // The status lines at the bottom of the screen, cut to fit.
        line[0][39] = line[1][39] = '\0';
        status_lines(line[0], line[1]);

        if (seconds % 5 == 0 && seconds <= 60) {
            log_line("watchdog: ", line[0]);
            log_line("watchdog: ", line[1]);
        }
    }
}

static void watchdog_start(void) {
    if (threadCreate(watchdog, NULL, 8 * 1024, 0x18, 0, true) == NULL) {
        plat_log("watchdog: could not create its thread");
    }
}

void plat_fatal(const char *fmt, ...) {
    static PrintConsole sTopConsole;
    char buffer[512];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    svcOutputDebugString(buffer, strlen(buffer));
    log_line("FATAL: ", buffer);

    // Shown as text on the top screen (the system error dialog does not appear in every emulator).
    consoleInit(GFX_TOP, &sTopConsole);
    printf("\x1b[2J\x1b[2;1H\x1b[31mDiddy Kong Racing could not start\x1b[0m\n\n%s\n\n\nPress START to exit.\n", buffer);

    if (threadGetCurrent() != NULL) {
        // Not the main thread: the main loop keeps the application responsive.
        for (;;) {
            svcSleepThread(1000000000LL);
        }
    }
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    exit(1);
}
