/*
 * Emulation of the libultra kernel: threads, message queues, events, interrupt masking and time.
 *
 * Every OSThread runs on a real host thread. On the 3DS all of them share one core and use strict priorities,
 * which matches the N64 scheduling model closely. Message queues keep their state in the OSMesgQueue
 * structure itself and are protected by one global lock.
 */
#include <ultra64.h>
#include "PR/os_internal.h"
#include "types.h"
#include "port.h"
#include "plat/plat.h"

#define THREAD_ENTRY(t) ((void (*)(void *))(uintptr_t)(t)->context.pc)
#define THREAD_ARG(t) ((void *) (uintptr_t)(t)->context.a0)
#define THREAD_PLAT(t) ((PlatThread *) (uintptr_t)(t)->context.s0)

static OSThread sMainThread;

/******************************** Lockstep ********************************/

/*
 * DKR_LOCKSTEP=1 makes runs repeatable, for comparing frames between builds (the host build's CI regression run).
 * Normally every N64 thread runs freely on its own host thread and retraces follow the wall clock, so how many
 * retraces pass during a frame (the game's logic step) and how threads interleave depend on the machine. In
 * lockstep the threads run one at a time like on the N64's single CPU: the highest priority thread that has work
 * runs until it blocks, the RCP finishes each task before the thread that started it goes on, and the VI raises
 * the next retrace only once no thread has anything left to do. The game then runs at a steady 30 updates per
 * second, as fast as the machine allows, and the clock it reads advances by exactly one 60th of a second per
 * retrace.
 */
#define MAX_THREADS 16

/* Started threads and the queue each one is blocked on. Protected by the kernel lock. */
static OSThread *sThreads[MAX_THREADS];
static OSMesgQueue *sThreadWaitQueue[MAX_THREADS];
static u8 sThreadWaitFull[MAX_THREADS];
static int (*sThreadWaitUntil[MAX_THREADS])(void);
static s32 sNumThreads;
/* The thread that holds the emulated CPU, or -1. */
static s32 sCpuOwner = -1;
static s32 sLockstep = -1;
static unsigned long long sLockstepRetraces;
static u32 sLockstepDmaBytes;

/* Loading still lets time pass: one retrace per this much PI DMA (the PI moves roughly 5 MB a second). */
#define LOCKSTEP_DMA_BYTES_PER_RETRACE 0x14000

int port_lockstep_enabled(void) {
    if (sLockstep < 0) {
        const char *env = plat_getenv("DKR_LOCKSTEP");
        sLockstep = env != NULL && env[0] != '\0' && env[0] != '0';
    }
    return sLockstep;
}

/* The rest of this section is called with the kernel lock held. */

static s32 thread_index(OSThread *t) {
    s32 i;
    for (i = 0; i < sNumThreads; i++) {
        if (sThreads[i] == t) {
            return i;
        }
    }
    return -1;
}

/* Whether the thread could run now: started, not stopped, and not blocked on something that isn't there yet. */
static int thread_ready(s32 i) {
    OSMesgQueue *mq = sThreadWaitQueue[i];
    if (sThreads[i]->state == OS_STATE_STOPPED) {
        return FALSE;
    }
    if (sThreadWaitUntil[i] != NULL) {
        return sThreadWaitUntil[i]();
    }
    if (mq == NULL) {
        return TRUE;
    }
    return sThreadWaitFull[i] ? mq->validCount < mq->msgCount : mq->validCount > 0;
}

/* Hands a free CPU to the highest priority ready thread (the first started one on a tie). */
static void cpu_pick(void) {
    s32 i, best = -1;
    if (sCpuOwner >= 0) {
        return;
    }
    for (i = 0; i < sNumThreads; i++) {
        if (thread_ready(i) && (best < 0 || sThreads[i]->priority > sThreads[best]->priority)) {
            best = i;
        }
    }
    if (best >= 0) {
        sCpuOwner = best;
        plat_kbroadcast();
    }
}

static void cpu_acquire(s32 i) {
    for (;;) {
        cpu_pick();
        if (sCpuOwner == i) {
            return;
        }
        plat_kwait();
    }
}

static void cpu_release(s32 i) {
    if (sCpuOwner == i) {
        sCpuOwner = -1;
        cpu_pick();
    }
}

void port_kwait_until(int (*ready)(void)) {
    s32 i = port_lockstep_enabled() ? thread_index((OSThread *) plat_get_current_os_thread()) : -1;
    if (i < 0) {
        while (!ready()) {
            plat_kwait();
        }
        return;
    }
    sThreadWaitUntil[i] = ready;
    cpu_release(i);
    cpu_acquire(i);
    sThreadWaitUntil[i] = NULL;
}

/* Called by a thread that stops for good. */
static void cpu_release_self(void) {
    if (port_lockstep_enabled()) {
        plat_klock();
        cpu_release(thread_index((OSThread *) plat_get_current_os_thread()));
        plat_kunlock();
    }
}

void port_lockstep_wait(void) {
    unsigned long long start = plat_time_ns();
    s32 i;

    plat_klock();
    for (;;) {
        cpu_pick();
        for (i = 0; i < sNumThreads && !thread_ready(i); i++) {
        }
        // Nothing happens before the game starts its first thread: start-up takes no time.
        if (sNumThreads > 0 && sCpuOwner < 0 && i == sNumThreads && port_rcp_idle()) {
            break;
        }
        if (sNumThreads > 0 && plat_time_ns() - start > 2000000000ULL) {
            // A thread polls instead of blocking. Let the retrace through rather than stall the run.
            port_log("lockstep: thread %d still running after 2 s, raising the retrace anyway",
                     sCpuOwner >= 0 ? (int) sThreads[sCpuOwner]->id : -1);
            break;
        }
        plat_kwait_timeout(1000000);
    }
    sLockstepRetraces++;
    plat_kunlock();
}

/* The console's clock as the game sees it. */
unsigned long long port_time_ns(void) {
    if (port_lockstep_enabled()) {
        return sLockstepRetraces * 1000000000ULL / 60;
    }
    return plat_time_ns();
}

/******************************** Threads ********************************/

static void thread_trampoline(void *arg) {
    OSThread *t = (OSThread *) arg;
    plat_set_current_os_thread(t);
    port_int_thread_init();
    if (port_lockstep_enabled() && thread_index(t) >= 0) {
        plat_klock();
        cpu_acquire(thread_index(t));
        plat_kunlock();
    }
    THREAD_ENTRY(t)(THREAD_ARG(t));
    // N64 threads are never supposed to return.
    t->state = OS_STATE_STOPPED;
    cpu_release_self();
    plat_thread_park();
}

void osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg, void *sp, OSPri pri) {
    (void) sp;
    t->id = id;
    t->priority = pri;
    t->next = NULL;
    t->queue = NULL;
    t->state = OS_STATE_STOPPED;
    t->flags = 0;
    t->context.pc = (u32) (uintptr_t) entry;
    t->context.a0 = (u64) (uintptr_t) arg;
    t->context.s0 = 0;
}

void osStartThread(OSThread *t) {
    char name[16];
    if (THREAD_PLAT(t) != NULL) {
        return;
    }
    plat_snprintf(name, sizeof(name), "n64thread%d", (int) t->id);
    t->state = OS_STATE_RUNNABLE;
    port_lockstep_enabled(); // Reads the setting now, rather than later with the kernel lock held.
    plat_klock();
    if (sNumThreads < MAX_THREADS) {
        sThreads[sNumThreads++] = t;
    }
    plat_kunlock();
    t->context.s0 = (u64) (uintptr_t) plat_thread_create(thread_trampoline, t, t->priority, PLAT_THREAD_GAME, name);
}

static OSThread *current_thread(void) {
    OSThread *t = (OSThread *) plat_get_current_os_thread();
    return (t != NULL) ? t : &sMainThread;
}

void osSetThreadPri(OSThread *t, OSPri pri) {
    if (t == NULL) {
        t = current_thread();
    }
    t->priority = pri;
    if (THREAD_PLAT(t) != NULL || t == current_thread()) {
        plat_thread_set_priority(t == current_thread() ? NULL : THREAD_PLAT(t), pri);
    }
}

OSPri osGetThreadPri(OSThread *t) {
    if (t == NULL) {
        t = current_thread();
    }
    return t->priority;
}

OSId osGetThreadId(OSThread *t) {
    if (t == NULL) {
        t = current_thread();
    }
    return t->id;
}

void osStopThread(OSThread *t) {
    // Stopping another thread isn't supported by the host thread APIs. The game only does this when it is
    // shutting down (reset), so stopping the calling thread is the only case that really matters.
    if (t == NULL || t == current_thread()) {
        current_thread()->state = OS_STATE_STOPPED;
        cpu_release_self();
        plat_thread_park();
    }
    t->state = OS_STATE_STOPPED;
}

void osDestroyThread(OSThread *t) {
    osStopThread(t);
}

void osYieldThread(void) {
    plat_sleep_ns(0);
}

/******************************** Interrupt mask ********************************/

/*
 * "Disabling interrupts" is emulated with a global lock. Holding it keeps every emulated interrupt source
 * (retrace, RCP completion, PI DMA) from delivering messages, and keeps other N64 threads out of their own
 * interrupt disabled sections, which is what code relying on osSetIntMask expects.
 * The lock is released while a thread blocks on a message queue, just like the N64 restores the interrupt
 * mask of whichever thread runs next.
 */
static __thread int sIntDisabled;

void port_int_thread_init(void) {
    sIntDisabled = FALSE;
}

static void int_disable(void) {
    if (!sIntDisabled) {
        plat_intlock_lock();
        sIntDisabled = TRUE;
    }
}

static void int_enable(void) {
    if (sIntDisabled) {
        sIntDisabled = FALSE;
        plat_intlock_unlock();
    }
}

int port_int_release(void) {
    int wasDisabled = sIntDisabled;
    int_enable();
    return wasDisabled;
}

void port_int_restore(int wasDisabled) {
    if (wasDisabled) {
        int_disable();
    }
}

OSIntMask osSetIntMask(OSIntMask mask) {
    OSIntMask prev = sIntDisabled ? OS_IM_NONE : OS_IM_ALL;
    if ((mask & OS_IM_CPU) == 0 || mask == OS_IM_NONE) {
        int_disable();
    } else {
        int_enable();
    }
    return prev;
}

OSIntMask osGetIntMask(void) {
    return sIntDisabled ? OS_IM_NONE : OS_IM_ALL;
}

u32 __osDisableInt(void) {
    u32 prev = sIntDisabled;
    int_disable();
    return !prev;
}

void __osRestoreInt(u32 enabled) {
    if (enabled) {
        int_enable();
    } else {
        int_disable();
    }
}

/*
 * Called after each PI DMA. Retraces otherwise only come between frames, so a long load would run without any,
 * and the audio thread, which works once per retrace, would not keep up with the music events the game queues
 * meanwhile (the music player's event queue overflows and the player stalls).
 */
void port_lockstep_dma(u32 numBytes) {
    s32 i;
    if (!port_lockstep_enabled() || sIntDisabled) {
        return;
    }
    plat_klock();
    i = thread_index((OSThread *) plat_get_current_os_thread());
    sLockstepDmaBytes += numBytes;
    if (i < 0 || sCpuOwner != i || sLockstepDmaBytes < LOCKSTEP_DMA_BYTES_PER_RETRACE) {
        plat_kunlock();
        return;
    }
    sLockstepDmaBytes -= LOCKSTEP_DMA_BYTES_PER_RETRACE;
    sLockstepRetraces++;
    plat_kunlock();
    port_vi_retrace();
    // Let the threads the retrace woke run first if they outrank this one, as the interrupt would on the N64.
    plat_klock();
    cpu_release(i);
    cpu_acquire(i);
    plat_kunlock();
}

/******************************** Message queues ********************************/

/* The queue each started thread is blocked on (for the 3DS watchdog's stall report). Kernel lock held to change. */
static OSMesgQueue *volatile sBlockedOn[MAX_THREADS];
static volatile u8 sBlockedSending[MAX_THREADS];

static void wait_on_queue_inner(OSMesgQueue *mq, int forSpace);
/* Queue waits that woke on their timeout with the queue already ready (see wait_on_queue_inner). */
volatile u32 gPortMissedWakeups;

/*
 * On the N64 a thread with interrupts disabled cannot be switched away from until it blocks: nothing can wake a
 * higher priority thread. Here the other threads still get their messages (the scheduler may already be running), so
 * a woken thread first waits for any interrupt disabled section to end. Without this the sound thread could run in the
 * middle of the game's (sound_play's sndp_allocate's) and edit the same lists, which tied them into a loop the sound
 * thread then spun in forever (60 fps freezes, issues #91, #93, #97 and #102). Not needed in lockstep, where only one
 * thread runs and the CPU only changes hands when a thread blocks.
 */
static void wait_for_int_enabled(void) {
    if (!sIntDisabled && !port_lockstep_enabled()) {
        plat_intlock_lock();
        plat_intlock_unlock();
    }
}

/* Blocks the calling thread until mq may have a message (or space, for a sender). Kernel lock held. */
static void wait_on_queue(OSMesgQueue *mq, int forSpace) {
    s32 t = thread_index((OSThread *) plat_get_current_os_thread());
    if (t >= 0) {
        sBlockedOn[t] = mq;
        sBlockedSending[t] = forSpace;
    }
    wait_on_queue_inner(mq, forSpace);
    if (t >= 0) {
        sBlockedOn[t] = NULL;
    }
}

/*
 * One line per N64 thread: its id, and the queue it is blocked on (address, messages held / size) or "running" if
 * it is not blocked on a queue (working, spinning, or waiting on something else).
 */
void port_describe_threads(char *buffer, s32 size) {
    s32 i, used = 0;
    buffer[0] = '\0';
    // No kernel lock: this runs in the middle of a stall, perhaps one where a thread holds it.
    for (i = 0; i < sNumThreads && used < size - 1; i++) {
        OSMesgQueue *mq = sBlockedOn[i];
        if (mq != NULL) {
            used += plat_snprintf(buffer + used, size - used, "%sthread %d %s %p (%d/%d)", i > 0 ? ", " : "",
                                  (int) sThreads[i]->id, sBlockedSending[i] ? "sending to" : "waiting on", (void *) mq,
                                  (int) mq->validCount, (int) mq->msgCount);
        } else {
            used += plat_snprintf(buffer + used, size - used, "%sthread %d running", i > 0 ? ", " : "",
                                  (int) sThreads[i]->id);
        }
    }
    if (used < size - 1) {
        plat_snprintf(buffer + used, size - used, ", %u missed wake-ups", (unsigned) gPortMissedWakeups);
    }
}

/*
 * Queue waits are bounded: a thread that sleeps past a message (a wake-up the 3DS condition variable lost, as in
 * #107 and #114, where the scheduler slept on a full interrupt queue and the game stopped) looks again on its own.
 * Those found with the queue ready on waking are counted, and the watchdog's thread line reports them.
 */
#define QUEUE_WAIT_NS 4000000ULL

/*
 * Roadmap P32 (DKR_QUEUE_WAKE=1, off: no gain in switch bench #246): every message used to wake every blocked N64 thread through
 * the kernel's one condition variable, each looking at its own queue again. With the switch on (outside lockstep), a
 * thread waiting on a queue sleeps on that queue's variable (plat_kwait_on_timeout) and a message wakes only those.
 * Nothing else waits on queues, so nothing else needs the queue's wake-ups; the bounded wait stays as it was.
 */
static int queue_wake_keyed(void) {
    // Read again only when a switch override changes (gPlatSwitchEpoch): this runs for every message.
    static int sEpoch = -1, sKeyed;
    if (sEpoch != gPlatSwitchEpoch) {
        sEpoch = gPlatSwitchEpoch;
        sKeyed = plat_switch("DKR_QUEUE_WAKE", 0) != 0;
    }
    return sKeyed && !port_lockstep_enabled();
}

static void queue_wake(OSMesgQueue *mq) {
    if (queue_wake_keyed()) {
        plat_kbroadcast_on(mq);
    } else {
        plat_kbroadcast();
    }
}

static void wait_on_queue_inner(OSMesgQueue *mq, int forSpace) {
    s32 i = port_lockstep_enabled() ? thread_index((OSThread *) plat_get_current_os_thread()) : -1;
    if (i < 0) {
        if ((queue_wake_keyed() ? plat_kwait_on_timeout(mq, QUEUE_WAIT_NS) : plat_kwait_timeout(QUEUE_WAIT_NS)) &&
            (forSpace ? mq->validCount < mq->msgCount : mq->validCount > 0)) {
            gPortMissedWakeups++;
        }
        return;
    }
    // In lockstep the CPU goes to the next thread, and comes back once this one is ready and has the highest priority.
    sThreadWaitQueue[i] = mq;
    sThreadWaitFull[i] = forSpace;
    cpu_release(i);
    cpu_acquire(i);
    sThreadWaitQueue[i] = NULL;
}

void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msg, s32 count) {
    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}

s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag) {
    int hadInt = (flag == OS_MESG_BLOCK) ? port_int_release() : FALSE;
    int waited = FALSE;

    plat_klock();
    while (mq->validCount >= mq->msgCount) {
        if (flag == OS_MESG_NOBLOCK) {
            plat_kunlock();
            port_int_restore(hadInt);
            return -1;
        }
        wait_on_queue(mq, TRUE);
        waited = TRUE;
    }
    mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    mq->validCount++;
    queue_wake(mq);
    plat_kunlock();
    port_int_restore(hadInt);
    if (waited) {
        wait_for_int_enabled();
    }
    return 0;
}

s32 osJamMesg(OSMesgQueue *mq, OSMesg msg, s32 flag) {
    int hadInt = (flag == OS_MESG_BLOCK) ? port_int_release() : FALSE;
    int waited = FALSE;

    plat_klock();
    while (mq->validCount >= mq->msgCount) {
        if (flag == OS_MESG_NOBLOCK) {
            plat_kunlock();
            port_int_restore(hadInt);
            return -1;
        }
        wait_on_queue(mq, TRUE);
        waited = TRUE;
    }
    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;
    queue_wake(mq);
    plat_kunlock();
    port_int_restore(hadInt);
    if (waited) {
        wait_for_int_enabled();
    }
    return 0;
}

s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag) {
    int hadInt = (flag == OS_MESG_BLOCK) ? port_int_release() : FALSE;
    int waited = FALSE;

    plat_klock();
    while (mq->validCount == 0) {
        if (flag == OS_MESG_NOBLOCK) {
            plat_kunlock();
            port_int_restore(hadInt);
            return -1;
        }
        wait_on_queue(mq, FALSE);
        waited = TRUE;
    }
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    queue_wake(mq);
    plat_kunlock();
    port_int_restore(hadInt);
    if (waited) {
        wait_for_int_enabled();
    }
    return 0;
}

/* Used by emulated hardware to post a message, like an interrupt handler would. Never blocks. */
/* Returns FALSE if the queue was full, like an interrupt's message on the N64, which is then lost. */
static s32 post_mesg(OSMesgQueue *mq, OSMesg msg) {
    s32 posted = FALSE;
    if (mq == NULL) {
        return TRUE;
    }
    plat_klock();
    if (mq->validCount < mq->msgCount) {
        mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
        mq->validCount++;
        queue_wake(mq);
        posted = TRUE;
    }
    plat_kunlock();
    return posted;
}

void port_post_mesg(OSMesgQueue *mq, OSMesg msg) {
    post_mesg(mq, msg);
}

/******************************** Events ********************************/

static struct {
    OSMesgQueue *queue;
    OSMesg msg;
} sEventTable[OS_NUM_EVENTS];

void osSetEventMesg(OSEvent event, OSMesgQueue *mq, OSMesg msg) {
    if (event < OS_NUM_EVENTS) {
        sEventTable[event].queue = mq;
        sEventTable[event].msg = msg;
    }
}

/* Raises an emulated hardware event. Waits for any thread that has interrupts disabled. */
/* Event messages lost to a full queue (logged by the 3DS watchdog with a hang). */
volatile u32 gPortEventsDropped;
/* Task-done messages waiting for room in a full queue right now. */
volatile u32 gPortEventsWaiting;

void port_raise_event(OSEvent event) {
    static u32 sWaits;
    s32 posted, waited = FALSE;
    if (event >= OS_NUM_EVENTS) {
        return;
    }
    for (;;) {
        plat_intlock_lock();
        posted = post_mesg(sEventTable[event].queue, sEventTable[event].msg);
        plat_intlock_unlock();
        // A lost task-done message leaves the scheduler waiting for it forever: the game and the sound both stop
        // (issues #91 and #93, at 60 fps, where the scheduler's 8-message queue holds less time). The RCP workers are
        // threads, not interrupts, so the done message waits for room instead.
        if (posted) {
            break;
        }
        if (event != OS_EVENT_SP && event != OS_EVENT_DP) {
            gPortEventsDropped++;
            break;
        }
        if (!waited) {
            waited = TRUE;
            gPortEventsWaiting++;
            if (sWaits++ % 64 == 0) {
                port_log("event queue full: %s done message waits for the scheduler (%u times)",
                         event == OS_EVENT_SP ? "SP" : "DP", (unsigned) sWaits);
            }
        }
        plat_sleep_ns(500000ULL);
    }
    if (waited) {
        gPortEventsWaiting--;
    }
}

/******************************** Time ********************************/

static unsigned long long sTimeBaseNs;
static OSTime sTimeBase;

/* The N64 count register runs at half the CPU clock: 46.875MHz. */
static OSTime ns_to_count(unsigned long long ns) {
    return (OSTime) ((ns / 1000ULL) * OS_CPU_COUNTER / 1000000ULL);
}

u32 osGetCount(void) {
    return (u32) ns_to_count(port_time_ns());
}

OSTime osGetTime(void) {
    return sTimeBase + ns_to_count(port_time_ns() - sTimeBaseNs);
}

void osSetTime(OSTime time) {
    sTimeBaseNs = port_time_ns();
    sTimeBase = time;
}

/******************************** Misc ********************************/

s32 osTvType = OS_TV_NTSC;
s32 osResetType = 0;
s32 osAppNMIBuffer[16];
u32 osMemSize = 0x800000;

void osInitialize(void) {
    plat_set_current_os_thread(&sMainThread);
    sMainThread.id = 0;
    sMainThread.priority = OS_PRIORITY_APPMAX;
    osSetTime(0);
}

void osWritebackDCache(void *vaddr, s32 nbytes) {
    (void) vaddr;
    (void) nbytes;
}

void osWritebackDCacheAll(void) {
}

void osInvalDCache(void *vaddr, s32 nbytes) {
    (void) vaddr;
    (void) nbytes;
}

void osInvalICache(void *vaddr, s32 nbytes) {
    (void) vaddr;
    (void) nbytes;
}

u32 osVirtualToPhysical(void *addr) {
    return (u32) (uintptr_t) addr;
}

void *osPhysicalToVirtual(u32 addr) {
    return (void *) (uintptr_t) addr;
}

void __osSpSetStatus(u32 status) {
    (void) status;
}

u32 __osSpGetStatus(void) {
    return SP_STATUS_HALT;
}

void osDpSetStatus(u32 status) {
    (void) status;
}

u32 osDpGetStatus(void) {
    return 0;
}

s32 osDpSetNextBuffer(void *buf, u64 size) {
    (void) buf;
    (void) size;
    return 0;
}

void rmonPrintf(const char *fmt, ...) {
    (void) fmt;
}

void __assert(const char *exp, const char *file, int line) {
    plat_fatal("Assertion failed: %s (%s:%d)", exp, file, line);
}

/* The active thread list is only walked by the crash handler. An empty list ends in a priority -1 sentinel. */
static OSThread sActiveQueueSentinel = { NULL, -1 };

OSThread *__osGetActiveQueue(void) {
    return &sActiveQueueSentinel;
}
