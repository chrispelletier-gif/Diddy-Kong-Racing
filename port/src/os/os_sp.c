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

static OSTask *sLoadedTask;
static OSTask *sTaskQueue[RCP_QUEUE_SIZE];
static volatile u32 sTaskQueueRead;
static volatile u32 sTaskQueueWrite;
static PlatSem *sTaskSem;
static s32 sRcpStarted;
static volatile s32 sGfxTasksPending;

static void rcp_thread(void *arg) {
    OSTask *task;

    (void) arg;
    port_int_thread_init();
    port_gfx_init();
    for (;;) {
        plat_sem_wait(sTaskSem);
        task = sTaskQueue[sTaskQueueRead % RCP_QUEUE_SIZE];
        sTaskQueueRead++;
        switch (task->t.type) {
            case M_GFXTASK:
                port_gfx_run_task(task);
                plat_klock();
                sGfxTasksPending--;
                plat_kbroadcast();
                plat_kunlock();
                port_raise_event(OS_EVENT_SP);
                port_raise_event(OS_EVENT_DP);
                break;
            case M_AUDTASK:
                port_audio_run_task(task);
                port_raise_event(OS_EVENT_SP);
                break;
            default:
                port_raise_event(OS_EVENT_SP);
                break;
        }
    }
}

void port_rcp_init(void) {
    if (!sRcpStarted) {
        sRcpStarted = TRUE;
        sTaskSem = plat_sem_create(0);
        plat_thread_create(rcp_thread, NULL, 0, PLAT_THREAD_RCP, "rcp");
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
    sTaskQueue[sTaskQueueWrite % RCP_QUEUE_SIZE] = task;
    sTaskQueueWrite++;
    plat_sem_post(sTaskSem);
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
    plat_klock();
    while (sGfxTasksPending > 0) {
        plat_kwait();
    }
    plat_kunlock();
}
