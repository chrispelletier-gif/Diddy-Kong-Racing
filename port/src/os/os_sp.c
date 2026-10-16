/*
 * Emulation of the signal processor (RSP) and display processor (RDP).
 * Tasks handed to osSpTaskStartGo are executed on a dedicated thread, which then raises the same SP and DP
 * interrupts the hardware would.
 */
#include <ultra64.h>
#include "types.h"
#include "port.h"
#include "plat/plat.h"

#define RCP_QUEUE_SIZE 8

/*
 * Two workers stand in for the RCP. Graphics tasks go to the "RDP" worker; since the RSP's share of a graphics task
 * is quick on the N64, the SP interrupt is raised as soon as the worker picks the task up and only the DP interrupt waits
 * for the frame to be drawn. That leaves the scheduler free to start audio tasks while a frame renders, which run on
 * the second worker, so a slow frame never starves the audio.
 */
typedef struct RcpWorker {
    OSTask *queue[RCP_QUEUE_SIZE];
    volatile u32 read;
    volatile u32 write;
    PlatSem *sem;
} RcpWorker;

static OSTask *sLoadedTask;
static RcpWorker sGfxWorker;
static RcpWorker sAudioWorker;
static s32 sRcpStarted;
static volatile s32 sGfxTasksPending;

static OSTask *worker_next(RcpWorker *worker) {
    OSTask *task;
    plat_sem_wait(worker->sem);
    task = worker->queue[worker->read % RCP_QUEUE_SIZE];
    worker->read++;
    return task;
}

static void worker_push(RcpWorker *worker, OSTask *task) {
    worker->queue[worker->write % RCP_QUEUE_SIZE] = task;
    worker->write++;
    plat_sem_post(worker->sem);
}

static void gfx_thread(void *arg) {
    OSTask *task;

    (void) arg;
    port_int_thread_init();
    port_gfx_init();
    for (;;) {
        task = worker_next(&sGfxWorker);
        port_raise_event(OS_EVENT_SP);
        port_gfx_run_task(task);
        plat_klock();
        sGfxTasksPending--;
        plat_kbroadcast();
        plat_kunlock();
        port_raise_event(OS_EVENT_DP);
    }
}

static void audio_thread(void *arg) {
    OSTask *task;

    (void) arg;
    port_int_thread_init();
    for (;;) {
        task = worker_next(&sAudioWorker);
        if (task->t.type == M_AUDTASK) {
            port_audio_run_task(task);
        }
        port_raise_event(OS_EVENT_SP);
    }
}

void port_rcp_init(void) {
    if (!sRcpStarted) {
        sRcpStarted = TRUE;
        sGfxWorker.sem = plat_sem_create(0);
        sAudioWorker.sem = plat_sem_create(0);
        plat_thread_create(gfx_thread, NULL, 0, PLAT_THREAD_RCP, "rdp");
        plat_thread_create(audio_thread, NULL, 0, PLAT_THREAD_RCP, "rsp");
    }
}

void osSpTaskLoad(OSTask *task) {
    sLoadedTask = task;
}

void osSpTaskStartGo(OSTask *task) {
    if (task == NULL) {
        task = sLoadedTask;
    }
    port_rcp_init();
    if (task->t.type == M_GFXTASK) {
        worker_push(&sGfxWorker, task);
    } else {
        worker_push(&sAudioWorker, task);
    }
}

void osSpTaskYield(void) {
    // Emulated tasks always run to completion.
}

OSYieldResult osSpTaskYielded(OSTask *task) {
    (void) task;
    return 0;
}

/* Called by the game when it hands a display list to the scheduler. */
void port_rcp_gfx_submitted(void) {
    plat_klock();
    sGfxTasksPending++;
    plat_kunlock();
}

/*
 * Waits until no graphics task is queued or running. The game sometimes frees memory that the display list being
 * drawn still uses, relying on the RCP having finished long before. This makes that safe however slow rendering is.
 */
void port_rcp_wait_idle(void) {
    int wasDisabled;
    if (sGfxTasksPending <= 0) {
        return;
    }
    // Callers may have "interrupts" disabled, but the frame can only finish if the VI and RCP threads can still
    // raise their events, so let them in while waiting (as the OS does for any blocking call).
    wasDisabled = port_int_release();
    plat_klock();
    while (sGfxTasksPending > 0) {
        plat_kwait();
    }
    plat_kunlock();
    port_int_restore(wasDisabled);
}
