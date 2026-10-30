/*
 * Emulation of the signal processor (RSP) and display processor (RDP).
 * Tasks handed to osSpTaskStartGo are executed on a dedicated thread, which then raises the same SP and DP
 * interrupts the hardware would.
 */
#include <ultra64.h>
#include "types.h"
#include "port.h"
#include "plat/plat.h"
#include "plat/profile.h"

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
// Graphics tasks handed over and finished so far (kernel lock), for waiting on the ones queued at a given moment.
static volatile u32 sGfxTasksSubmitted, sGfxTasksFinished;
/* Each display list's logic step (gPortLogicStep), by the order they were handed over; they are drawn in that order. */
#define GFX_STEP_RING 8
static u8 sGfxSteps[GFX_STEP_RING];
/* Tasks handed to the workers whose completion has not been raised yet (kernel lock). */
static s32 sRcpTasksBusy;
/* What the audio worker is doing, for the 3DS watchdog's stall report. */
static const char *volatile sAudioStage = "not started";

static OSTask *worker_next(RcpWorker *worker) {
    OSTask *task;
    plat_sem_wait(worker->sem);
    task = worker->queue[worker->read % RCP_QUEUE_SIZE];
    worker->read++;
    return task;
}

static void worker_push(RcpWorker *worker, OSTask *task) {
    plat_klock();
    sRcpTasksBusy++;
    plat_kunlock();
    worker->queue[worker->write % RCP_QUEUE_SIZE] = task;
    worker->write++;
    plat_sem_post(worker->sem);
}

/* Called after a task's completion event is raised, so lockstep never sees the RCP idle before it. */
static void task_done(void) {
    plat_klock();
    sRcpTasksBusy--;
    plat_kbroadcast();
    plat_kunlock();
}

void port_describe_rcp(char *buffer, s32 size) {
    plat_snprintf(buffer, size, "rcp: gfx tasks %u taken of %u, audio tasks %u taken of %u, %d busy, %d gfx pending, "
                  "audio worker %s", (unsigned) sGfxWorker.read, (unsigned) sGfxWorker.write,
                  (unsigned) sAudioWorker.read, (unsigned) sAudioWorker.write, (int) sRcpTasksBusy,
                  (int) sGfxTasksPending, sAudioStage);
}

int port_rcp_idle(void) {
    return sRcpTasksBusy == 0;
}

s32 port_render_ahead(void) {
    return plat_render_ahead();
}

/* The next of the game's per-frame geometry copies (FRAME_FLIP): two in turn as on the N64, three when two frames may be in flight. */
int port_frame_flip(int flip) {
    return (flip + 1) % (plat_render_ahead() + 1);
}

static void gfx_thread(void *arg) {
    OSTask *task;

    (void) arg;
    port_int_thread_init();
    port_gfx_init();
    for (;;) {
        unsigned long long start;
        gPlatGfxStage = "waiting for a display list";
        task = worker_next(&sGfxWorker);
        port_raise_event(OS_EVENT_SP);
        plat_set_frame_step(sGfxSteps[sGfxTasksFinished % GFX_STEP_RING]);
        start = plat_time_ns();
        port_gfx_run_task(task);
        prof_render_task(plat_time_ns() - start);
        port_load_frame_done();
        gPlatCounters[PLAT_COUNTER_GFX_DONE]++;
        gPlatGfxStage = "raising the DP interrupt";
        plat_klock();
        sGfxTasksPending--;
        sGfxTasksFinished++;
        plat_kbroadcast();
        plat_kunlock();
        port_raise_event(OS_EVENT_DP);
        task_done();
    }
}

static void audio_thread(void *arg) {
    OSTask *task;

    (void) arg;
    port_int_thread_init();
    for (;;) {
        sAudioStage = "waiting for a task";
        task = worker_next(&sAudioWorker);
        sAudioStage = "mixing";
        if (task->t.type == M_AUDTASK) {
            unsigned long long start = plat_time_ns();
            port_audio_run_task(task);
            prof_add(PROF_AUDIO, plat_time_ns() - start);
            gPlatCounters[PLAT_COUNTER_AUDIO]++;
        }
        sAudioStage = "raising the SP interrupt";
        port_raise_event(OS_EVENT_SP);
        task_done();
    }
}

void port_rcp_init(void) {
    if (!sRcpStarted) {
        sRcpStarted = TRUE;
        sGfxWorker.sem = plat_sem_create(0);
        sAudioWorker.sem = plat_sem_create(0);
        plat_thread_create(gfx_thread, NULL, 0, PLAT_THREAD_RCP, "rdp");
        plat_thread_create(audio_thread, NULL, 0, PLAT_THREAD_AUDIO, "rsp");
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
    if (port_lockstep_enabled()) {
        // Lockstep: the task finishes, and raises its interrupts, before the caller goes on.
        int wasDisabled = port_int_release();
        plat_klock();
        while (sRcpTasksBusy > 0) {
            plat_kwait();
        }
        plat_kunlock();
        port_int_restore(wasDisabled);
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
    gPlatCounters[PLAT_COUNTER_GFX_QUEUED]++;
    plat_klock();
    sGfxTasksPending++;
    sGfxSteps[sGfxTasksSubmitted % GFX_STEP_RING] = (u8) gPortLogicStep;
    sGfxTasksSubmitted++;
    plat_kunlock();
}

/*
 * Waits until the graphics tasks queued or running now have finished. The game sometimes frees memory that the
 * display list being drawn still uses, relying on the RCP having finished long before. This makes that safe however
 * slow rendering is. Tasks handed over later can't use memory freed now, so they aren't waited for: the tracks
 * menu's background loader frees a lot while the game thread keeps queueing menu frames, and waiting for no task at
 * all could take seconds, or forever with frames rendered ahead.
 */
static volatile u32 sGfxWaitTarget; // The newest task any waiter needs finished (kernel lock).

static int gfx_idle(void) {
    return (s32) (sGfxTasksFinished - sGfxWaitTarget) >= 0;
}

void port_rcp_wait_idle(void) {
    int wasDisabled;
    if (sGfxTasksPending <= 0) {
        return;
    }
    // Callers may have "interrupts" disabled, but the frame can only finish if the VI and RCP threads can still
    // raise their events, so let them in while waiting (as the OS does for any blocking call).
    wasDisabled = port_int_release();
    plat_klock();
    if ((s32) (sGfxTasksSubmitted - sGfxWaitTarget) > 0) {
        sGfxWaitTarget = sGfxTasksSubmitted;
    }
    port_kwait_until(gfx_idle);
    plat_kunlock();
    port_int_restore(wasDisabled);
}
