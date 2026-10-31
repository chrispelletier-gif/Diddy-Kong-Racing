/* POSIX implementation of the platform layer. Used by the host (Linux/macOS) test build. */
#define _GNU_SOURCE
#include "plat.h"
#include "recorder.h"
#include "race_hud.h"
#include "setup_screen.h"
#include "bottom_screen.h"

#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <malloc.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

struct PlatThread {
    pthread_t handle;
    PlatThreadFunc func;
    void *arg;
};

struct PlatSem {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
};

static pthread_mutex_t sKernelLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sKernelCond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t sKeyedConds[PLAT_KEYED_CONDS];
static pthread_once_t sKeyedOnce = PTHREAD_ONCE_INIT;

static void keyed_init(void) {
    int i;
    for (i = 0; i < PLAT_KEYED_CONDS; i++) {
        pthread_cond_init(&sKeyedConds[i], NULL);
    }
}
static pthread_mutex_t sIntLock = PTHREAD_MUTEX_INITIALIZER;
static __thread void *sCurrentOsThread;
static unsigned long long sNextVblank;

void plat_init(void) {
}

unsigned long long plat_idle_ns(void) {
    return 0;
}

unsigned long long plat_render_idle_ns(void) {
    return 0;
}

int plat_old_3ds(void) {
    return 0;
}

unsigned long long plat_screen_busy_ns(void) {
    return 0;
}

unsigned long plat_screen_draws(void) {
    return 0;
}

void plat_shutdown(void) {
}

void plat_overlay_show(const char *text) {
    (void) text;
}

void plat_overlay_graph(const unsigned char *halfMs, const unsigned char *kinds, int count) {
    (void) halfMs;
    (void) kinds;
    (void) count;
}

int plat_overlay_tap(int x, int y) {
    (void) x;
    (void) y;
    return 0;
}

void host_bottom_preview(const char *dir);

void host_bottom_race_start(void);
void host_bottom_race(const char *dir, const RaceHud *hud);
void host_bottom_setup(const char *dir, const SetupScreen *screen);

/*
 * DKR_BOTTOM_PREVIEW=dir: draws the 3DS bottom screen's views to PNG files in dir, then exits.
 * DKR_BOTTOM_RACE=dir: draws the bottom screen's race HUD from the race being played to dir every second or so.
 */
void plat_rom_loaded(void) {
    const char *dir = getenv("DKR_BOTTOM_PREVIEW");
    if (dir != NULL) {
        host_bottom_preview(dir);
        exit(0);
    }
    if (getenv("DKR_BOTTOM_RACE") != NULL) {
        host_bottom_race_start();
    }
}

/* The race HUD of the 3DS bottom screen (port_race_hud.c): only drawn for DKR_BOTTOM_RACE. */
int plat_race_hud_screen(void) {
    return getenv("DKR_BOTTOM_RACE") != NULL;
}

int plat_race_hud_covered(void) {
    return 0;
}

void plat_set_bottom_style(int style) {
    bs_set_style(style);
}

void plat_set_race_hud(const RaceHud *hud) {
    const char *dir = getenv("DKR_BOTTOM_RACE");
    if (dir != NULL && hud != NULL) {
        host_bottom_race(dir, hud);
    }
}

/*
 * The first-run setup (port_setup.c): only for DKR_SETUP=1, so that tests never meet it; DKR_SETUP_FRENCH=1 starts it
 * in French, as a French 3DS would. DKR_SETUP_SHOTS=dir draws its pages to PNG files.
 */
int plat_first_run_setup(void) {
    return getenv("DKR_SETUP") != NULL && atoi(getenv("DKR_SETUP")) != 0;
}

int plat_system_french(void) {
    return getenv("DKR_SETUP_FRENCH") != NULL && atoi(getenv("DKR_SETUP_FRENCH")) != 0;
}

int plat_can_60fps(void) {
    return getenv("DKR_SETUP_OLD_3DS") == NULL;
}

void plat_set_setup_screen(const SetupScreen *screen) {
    const char *dir = getenv("DKR_SETUP_SHOTS");
    if (dir != NULL) {
        host_bottom_setup(dir, screen);
    }
}

void plat_request_close_menu(void) {}

int plat_take_setup_tap(void) {
    return SETUP_HIT_NONE;
}

int plat_picture_style(void) {
    return plat_60fps_setting();
}

void plat_set_picture_style(int smooth) {
    plat_set_60fps_setting(smooth);
    plat_set_gpu_config(smooth ? PLAT_AA_OFF : PLAT_AA_4X);
}

/* The PC build's picture keeps DKR_WIDESCREEN's style; the setup's choice is only kept. */
static int sWidescreenStyle = -1;

int plat_widescreen_style(void) {
    if (sWidescreenStyle < 0) {
        const char *env = getenv("DKR_WIDESCREEN");
        sWidescreenStyle = env != NULL ? atoi(env) : 2;
    }
    return sWidescreenStyle;
}

void plat_set_widescreen_style(int style) {
    sWidescreenStyle = style;
}

/* No bug reports on the host: the results summary goes to the log instead. */
void plat_send_test_results(const char *title, const char *summary) {
    fprintf(stderr, "test results: %s\n%s\n", title, summary);
}

static int sAntialias = PLAT_AA_4X;

void plat_set_gpu_config(int aa) {
    sAntialias = aa < 0 ? PLAT_AA_4X : aa;
}

float plat_aspect_scale(void) {
    static int sWide = -1;
    if (sWide < 0) {
        const char *env = getenv("DKR_WIDESCREEN");
        sWide = env != NULL && atoi(env) != 0;
    }
    return sWide ? 1.25f : 1.0f;
}

float plat_fov_tan_scale(void) {
    // DKR_WIDESCREEN picks the style to preview, as the 3DS's setting does: 1 Wide, 2 Balanced, 3 Zoomed.
    static const float sTanScale[4] = { 1.0f, 1.0f, 0.894427f, 0.8f };
    static int sStyle = -1;
    if (sStyle < 0) {
        const char *env = getenv("DKR_WIDESCREEN");
        sStyle = env != NULL ? atoi(env) : 0;
        sStyle = sStyle >= 0 && sStyle <= 3 ? sStyle : 0;
    }
    return sTanScale[sStyle];
}

float plat_camera_fov(float fovy) {
    float scale = plat_fov_tan_scale();
    if (scale == 1.0f) {
        return fovy;
    }
    // Half the angle in radians, and the whole angle back in degrees.
    return 2.0f * atanf(tanf(fovy * (3.14159265f / 360.0f)) * scale) * (180.0f / 3.14159265f);
}

int plat_antialias(void) {
    return sAntialias;
}

static void *thread_entry(void *arg) {
    PlatThread *thread = (PlatThread *) arg;
    thread->func(thread->arg);
    return NULL;
}

PlatThread *plat_thread_create(PlatThreadFunc func, void *arg, int n64Priority, int kind, const char *name) {
    PlatThread *thread = calloc(1, sizeof(PlatThread));
    pthread_attr_t attr;

    (void) n64Priority;
    (void) kind;
    thread->func = func;
    thread->arg = arg;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread->handle, &attr, thread_entry, thread) != 0) {
        plat_fatal("Could not create thread %s", name);
    }
#ifdef __linux__
    pthread_setname_np(thread->handle, name);
#endif
    pthread_attr_destroy(&attr);
    return thread;
}

void plat_split_render_cores(int on) {
    (void) on;
}

void plat_thread_set_priority(PlatThread *thread, int n64Priority) {
    (void) thread;
    (void) n64Priority;
}

void plat_thread_park(void) {
    for (;;) {
        plat_sleep_ns(1000000000ULL);
    }
}

void plat_set_current_os_thread(void *thread) {
    sCurrentOsThread = thread;
}

void *plat_get_current_os_thread(void) {
    return sCurrentOsThread;
}

unsigned long long plat_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long) ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

unsigned long long plat_ticks(void) {
    return plat_time_ns();
}

unsigned long long plat_ticks_per_second(void) {
    return 1000000000ULL;
}

void plat_sleep_ns(unsigned long long ns) {
    struct timespec ts;
    ts.tv_sec = ns / 1000000000ULL;
    ts.tv_nsec = ns % 1000000000ULL;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

void plat_klock(void) {
    pthread_mutex_lock(&sKernelLock);
}

void plat_kunlock(void) {
    pthread_mutex_unlock(&sKernelLock);
}

void plat_kwait(void) {
    pthread_cond_wait(&sKernelCond, &sKernelLock);
}

int plat_kwait_timeout(unsigned long long ns) {
    struct timespec ts;
    unsigned long long deadline;

    clock_gettime(CLOCK_REALTIME, &ts);
    deadline = (unsigned long long) ts.tv_sec * 1000000000ULL + ts.tv_nsec + ns;
    ts.tv_sec = deadline / 1000000000ULL;
    ts.tv_nsec = deadline % 1000000000ULL;
    return pthread_cond_timedwait(&sKernelCond, &sKernelLock, &ts) == ETIMEDOUT;
}

void plat_kbroadcast(void) {
    pthread_cond_broadcast(&sKernelCond);
}

int plat_kwait_on_timeout(const void *key, unsigned long long ns) {
    struct timespec ts;
    unsigned long long deadline;

    pthread_once(&sKeyedOnce, keyed_init);
    clock_gettime(CLOCK_REALTIME, &ts);
    deadline = (unsigned long long) ts.tv_sec * 1000000000ULL + ts.tv_nsec + ns;
    ts.tv_sec = deadline / 1000000000ULL;
    ts.tv_nsec = deadline % 1000000000ULL;
    return pthread_cond_timedwait(&sKeyedConds[PLAT_KEY_SLOT(key)], &sKernelLock, &ts) == ETIMEDOUT;
}

void plat_kbroadcast_on(const void *key) {
    pthread_once(&sKeyedOnce, keyed_init);
    pthread_cond_broadcast(&sKeyedConds[PLAT_KEY_SLOT(key)]);
}

void plat_intlock_lock(void) {
    pthread_mutex_lock(&sIntLock);
}

void plat_intlock_unlock(void) {
    pthread_mutex_unlock(&sIntLock);
}

PlatSem *plat_sem_create(int initial) {
    PlatSem *sem = calloc(1, sizeof(PlatSem));
    pthread_mutex_init(&sem->mutex, NULL);
    pthread_cond_init(&sem->cond, NULL);
    sem->count = initial;
    return sem;
}

void plat_sem_post(PlatSem *sem) {
    pthread_mutex_lock(&sem->mutex);
    sem->count++;
    pthread_cond_signal(&sem->cond);
    pthread_mutex_unlock(&sem->mutex);
}

void plat_sem_wait(PlatSem *sem) {
    pthread_mutex_lock(&sem->mutex);
    while (sem->count == 0) {
        pthread_cond_wait(&sem->cond, &sem->mutex);
    }
    sem->count--;
    pthread_mutex_unlock(&sem->mutex);
}

int plat_sem_try_wait(PlatSem *sem) {
    int taken;
    pthread_mutex_lock(&sem->mutex);
    taken = sem->count > 0;
    if (taken) {
        sem->count--;
    }
    pthread_mutex_unlock(&sem->mutex);
    return taken;
}

void plat_wait_vblank(void) {
    unsigned long long now = plat_time_ns();
    const unsigned long long period = 1000000000ULL / 60;

    if (sNextVblank == 0 || now > sNextVblank + period * 4) {
        sNextVblank = now;
    }
    sNextVblank += period;
    if (sNextVblank > now) {
        plat_sleep_ns(sNextVblank - now);
    }
}

const char *const *plat_rom_search_dirs(void) {
    static const char *sDirs[2];
    sDirs[0] = plat_data_dir();
    sDirs[1] = NULL;
    return sDirs;
}

const char *plat_data_dir(void) {
    const char *dir = getenv("DKR_DATA_DIR");
    return dir != NULL ? dir : ".";
}

void plat_log(const char *fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    fprintf(stderr, "%s\n", buffer);
    rec_log_line("", buffer);
}

void plat_log_keep(void) {
}

void plat_log_copy_to(const char *path) {
    (void) path;
}

unsigned int plat_log_total(void) {
    return 0;
}

int plat_log_recent(char *out, int size) {
    if (size > 0) {
        out[0] = '\0';
    }
    return 0;
}

void plat_status(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

void plat_fatal(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fputs("FATAL: ", stderr);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    abort();
}

void plat_log_memory(const char *when) {
#if defined(__GLIBC__)
    struct mallinfo2 info = mallinfo2();
    plat_log("memory %s: heap in use %lu KB", when, (unsigned long) (info.uordblks / 1024));
#else
    (void) when;
#endif
}
