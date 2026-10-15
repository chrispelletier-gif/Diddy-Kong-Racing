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
    PLAT_THREAD_RCP,      /* Emulated RSP/RDP. May run on a different core. */
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
void *plat_malloc(size_t size);
void plat_free(void *ptr);
int plat_snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Directory where the ROM and save files live. */
const char *plat_data_dir(void);

void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void plat_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

#ifdef __cplusplus
}
#endif

#endif
