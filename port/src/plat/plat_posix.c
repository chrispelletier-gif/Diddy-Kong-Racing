/* POSIX implementation of the platform layer. Used by the host (Linux/macOS) test build. */
#define _GNU_SOURCE
#include "plat.h"

#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
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
static pthread_mutex_t sIntLock = PTHREAD_MUTEX_INITIALIZER;
static __thread void *sCurrentOsThread;
static unsigned long long sNextVblank;

void plat_init(void) {
}

unsigned long long plat_idle_ns(void) {
    return 0;
}

void plat_shutdown(void) {
}

void plat_overlay_show(const char *text) {
    (void) text;
}

void plat_send_test_results(const char *title, const char *summary) {
    (void) title;
    (void) summary;
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
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
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
