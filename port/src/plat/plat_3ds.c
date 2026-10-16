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

    // On a New 3DS the extra cores are free for us, so the emulated RCP gets a core of its own.
    APT_CheckNew3DS(&isNew3DS);
    if (isNew3DS) {
        osSetSpeedupEnable(true);
        sRcpCore = 2;
    } else {
        sRcpCore = 0;
    }
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
        default:
            prio = prio_from_n64(n64Priority);
            core = 0;
            break;
    }
    thread->handle = threadCreate(thread_entry, thread, stackSize, prio, core, true);
    if (thread->handle == NULL && core != 0) {
        thread->handle = threadCreate(thread_entry, thread, stackSize, prio, 0, true);
    }
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

const char *plat_data_dir(void) {
    return "sdmc:/3ds/dkr";
}

/* Creating sdmc:/3ds/dkr/debug turns on logging to sdmc:/3ds/dkr/log.txt. */
static FILE *log_file(void) {
    static int sChecked;
    static FILE *sFile;
    if (!sChecked) {
        FILE *flag = fopen("sdmc:/3ds/dkr/debug", "rb");
        sChecked = 1;
        if (flag != NULL) {
            fclose(flag);
            sFile = fopen("sdmc:/3ds/dkr/log.txt", "w");
        }
    }
    return sFile;
}

void plat_log(const char *fmt, ...) {
    char buffer[256];
    FILE *file;
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    svcOutputDebugString(buffer, strlen(buffer));
    file = log_file();
    if (file != NULL) {
        fprintf(file, "%s\n", buffer);
        fflush(file);
    }
#ifdef PORT_DEBUG_CONSOLE
    printf("%s\n", buffer);
#endif
}

void plat_fatal(const char *fmt, ...) {
    char buffer[512];
    va_list args;
    errorConf err;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    svcOutputDebugString(buffer, strlen(buffer));

    errorInit(&err, ERROR_TEXT, CFG_LANGUAGE_EN);
    errorText(&err, buffer);
    errorDisp(&err);
    for (;;) {
        svcSleepThread(1000000000LL);
    }
}
