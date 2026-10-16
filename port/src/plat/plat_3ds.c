/* Nintendo 3DS (libctru) implementation of the platform layer. */
#include "plat.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * All emulated N64 threads run on the application core so that, like on the N64, only one of them executes
 * at any time and a higher priority thread preempts a lower priority one as soon as it becomes ready.
 * 3DS thread priorities run from 0x18 (highest) to 0x3F (lowest), and the main thread normally sits at 0x30.
 */
#define PRIO_HARDWARE 0x19
#define PRIO_RCP 0x1A
#define PRIO_GAME_HIGHEST 0x1B
#define PRIO_GAME_LOWEST 0x3F

struct PlatThread {
    Thread handle;
    PlatThreadFunc func;
    void *arg;
};

struct PlatSem {
    LightSemaphore sem;
};

static LightLock sKernelLock;
static CondVar sKernelCond;
static LightLock sIntLock;
static __thread void *sCurrentOsThread;
static int sRcpCore;
static int sAudioCore;
static LightLock sConsoleLock;

static void watchdog_start(void);

static int prio_from_n64(int n64Priority) {
    int prio;
    // N64 priorities: 0 (idle) to 255 (max). The game itself uses 0..150 plus a couple of OS threads at 254/255.
    if (n64Priority >= 150) {
        return PRIO_GAME_HIGHEST;
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
    LightLock_Init(&sIntLock);
    LightLock_Init(&sConsoleLock);

    // On a New 3DS the extra cores are free for us, so the emulated RCP gets a core of its own.
    // DKR_SINGLE_CORE=1 keeps everything on the application core, as on an Old 3DS.
    APT_CheckNew3DS(&isNew3DS);
    if (isNew3DS) {
        osSetSpeedupEnable(true);
    }
    sRcpCore = (isNew3DS && plat_getenv("DKR_SINGLE_CORE") == NULL) ? 2 : 0;
    plat_status("%s, renderer on core %d", isNew3DS ? "New 3DS" : "Old 3DS", sRcpCore);
    // The scheduler cannot start the next frame while an audio task is running, so audio tasks must finish quickly:
    // they run next to the renderer, at a higher priority. (The system core, which only gives applications a
    // share of its time, made them take several times longer.)
    sAudioCore = sRcpCore;
    watchdog_start();
    {
        // citro3d is set up (and the GPU self test run) on the main thread, as in SM64's 3DS port.
        extern void gfxb_init(void);
        gfxb_init();
    }
}

/*
 * With DKR_PROFILE set, a lowest priority thread on the application core measures how much time is left over.
 * It spins in short slices and counts how long each slice took.
 */
static volatile unsigned long long sIdleNs;

static void idle_meter(void *arg) {
    (void) arg;
    for (;;) {
        unsigned long long start = plat_time_ns(), now;
        do {
            now = plat_time_ns();
        } while (now - start < 200000ULL);
        // A slice that took much longer than 0.2ms means higher priority threads ran in between.
        sIdleNs += 200000ULL;
    }
}

unsigned long long plat_idle_ns(void) {
    static int sStarted;
    if (!sStarted) {
        sStarted = 1;
        threadCreate(idle_meter, NULL, 4096, 0x3F, 0, true);
    }
    return sIdleNs;
}

void plat_shutdown(void) {
}

static void thread_entry(void *arg) {
    PlatThread *thread = (PlatThread *) arg;
    thread->func(thread->arg);
}

PlatThread *plat_thread_create(PlatThreadFunc func, void *arg, int n64Priority, int kind, const char *name) {
    PlatThread *thread = calloc(1, sizeof(PlatThread));
    int prio, core;
    size_t stackSize = 64 * 1024;

    thread->func = func;
    thread->arg = arg;
    switch (kind) {
        case PLAT_THREAD_HARDWARE:
            prio = PRIO_HARDWARE;
            core = 0;
            stackSize = 16 * 1024;
            break;
        case PLAT_THREAD_RCP:
            prio = PRIO_RCP;
            core = sRcpCore;
            stackSize = 128 * 1024;
            break;
        case PLAT_THREAD_AUDIO:
            // Above the renderer: 3DS threads of equal priority never preempt each other, so an audio task that
            // arrives while a frame is being drawn would otherwise wait for the whole frame and the sound would
            // run dry.
            prio = PRIO_HARDWARE;
            core = sAudioCore;
            stackSize = 64 * 1024;
            break;
        default:
            prio = prio_from_n64(n64Priority);
            core = 0;
            break;
    }
    thread->handle = threadCreate(thread_entry, thread, stackSize, prio, core, true);
    if (thread->handle == NULL && core != 0) {
        plat_log("thread %s: core %d unavailable, using core 0", name, core);
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

void plat_sleep_ns(unsigned long long ns) {
    svcSleepThread((s64) ns);
}

void plat_klock(void) {
    LightLock_Lock(&sKernelLock);
}

void plat_kunlock(void) {
    LightLock_Unlock(&sKernelLock);
}

void plat_kwait(void) {
    CondVar_Wait(&sKernelCond, &sKernelLock);
}

int plat_kwait_timeout(unsigned long long ns) {
    return CondVar_WaitTimeout(&sKernelCond, &sKernelLock, (s64) ns) != 0;
}

void plat_kbroadcast(void) {
    CondVar_Broadcast(&sKernelCond);
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
    file = log_file();
    if (file != NULL) {
        fprintf(file, "%7llu %s%s\n", plat_time_ns() / 1000000ULL, prefix, text);
        fflush(file);
        sLogLines++;
    }
    LightLock_Unlock(&sLogLock);
}

void plat_log(const char *fmt, ...) {
    char buffer[256];
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

/* Makes what the console wrote visible: nothing else flushes the bottom screen's framebuffer from the CPU cache. */
static void console_flush(void) {
    u16 width, height;
    u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &width, &height);
    GSPGPU_FlushDataCache(fb, width * height * 2);
}

/* The bottom screen console is the current one (see main_3ds.c). */
void plat_status(const char *fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    log_line("", buffer);
    LightLock_Lock(&sConsoleLock);
    printf("  %s\n", buffer);
    console_flush();
    LightLock_Unlock(&sConsoleLock);
}

/*
 * The watchdog shows the progress counters (plat.h) at the bottom of the bottom screen once a second, and logs them
 * every few seconds for the first minute. Its thread has the highest priority an application can use, so it keeps
 * running even if a game thread never yields; if its line stops updating, the whole system is stuck (for example
 * the GPU).
 */
static void watchdog(void *arg) {
    unsigned int last[PLAT_COUNTER_COUNT];
    int seconds = 0;

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
        snprintf(line[1], sizeof(line[1]), "rdp: %.20s gap %u drop %u", gPlatGfxStage,
                 now[PLAT_COUNTER_AUDIO_UNDERRUN], now[PLAT_COUNTER_AUDIO_DROPPED]);
        memcpy(last, now, sizeof(last));

        // Two fixed rows at the bottom; the cursor is put back for the start-up lines.
        LightLock_Lock(&sConsoleLock);
        printf("\x1b[s\x1b[29;1H\x1b[2K%.39s\x1b[30;1H\x1b[2K%.39s\x1b[u", line[0], line[1]);
        console_flush();
        LightLock_Unlock(&sConsoleLock);

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
