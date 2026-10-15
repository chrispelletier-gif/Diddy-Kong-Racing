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

/******************************** Threads ********************************/

#define THREAD_ENTRY(t) ((void (*)(void *))(uintptr_t)(t)->context.pc)
#define THREAD_ARG(t) ((void *) (uintptr_t)(t)->context.a0)
#define THREAD_PLAT(t) ((PlatThread *) (uintptr_t)(t)->context.s0)

static OSThread sMainThread;

static void thread_trampoline(void *arg) {
    OSThread *t = (OSThread *) arg;
    plat_set_current_os_thread(t);
    port_int_thread_init();
    THREAD_ENTRY(t)(THREAD_ARG(t));
    // N64 threads are never supposed to return.
    t->state = OS_STATE_STOPPED;
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

/******************************** Message queues ********************************/

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

    plat_klock();
    while (mq->validCount >= mq->msgCount) {
        if (flag == OS_MESG_NOBLOCK) {
            plat_kunlock();
            port_int_restore(hadInt);
            return -1;
        }
        plat_kwait();
    }
    mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    mq->validCount++;
    plat_kbroadcast();
    plat_kunlock();
    port_int_restore(hadInt);
    return 0;
}

s32 osJamMesg(OSMesgQueue *mq, OSMesg msg, s32 flag) {
    int hadInt = (flag == OS_MESG_BLOCK) ? port_int_release() : FALSE;

    plat_klock();
    while (mq->validCount >= mq->msgCount) {
        if (flag == OS_MESG_NOBLOCK) {
            plat_kunlock();
            port_int_restore(hadInt);
            return -1;
        }
        plat_kwait();
    }
    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;
    plat_kbroadcast();
    plat_kunlock();
    port_int_restore(hadInt);
    return 0;
}

s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag) {
    int hadInt = (flag == OS_MESG_BLOCK) ? port_int_release() : FALSE;

    plat_klock();
    while (mq->validCount == 0) {
        if (flag == OS_MESG_NOBLOCK) {
            plat_kunlock();
            port_int_restore(hadInt);
            return -1;
        }
        plat_kwait();
    }
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    plat_kbroadcast();
    plat_kunlock();
    port_int_restore(hadInt);
    return 0;
}

/* Used by emulated hardware to post a message, like an interrupt handler would. Never blocks. */
void port_post_mesg(OSMesgQueue *mq, OSMesg msg) {
    if (mq == NULL) {
        return;
    }
    plat_klock();
    if (mq->validCount < mq->msgCount) {
        mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
        mq->validCount++;
        plat_kbroadcast();
    }
    plat_kunlock();
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
void port_raise_event(OSEvent event) {
    if (event >= OS_NUM_EVENTS) {
        return;
    }
    plat_intlock_lock();
    port_post_mesg(sEventTable[event].queue, sEventTable[event].msg);
    plat_intlock_unlock();
}

/******************************** Time ********************************/

static unsigned long long sTimeBaseNs;
static OSTime sTimeBase;

/* The N64 count register runs at half the CPU clock: 46.875MHz. */
static OSTime ns_to_count(unsigned long long ns) {
    return (OSTime) ((ns / 1000ULL) * OS_CPU_COUNTER / 1000000ULL);
}

u32 osGetCount(void) {
    return (u32) ns_to_count(plat_time_ns());
}

OSTime osGetTime(void) {
    return sTimeBase + ns_to_count(plat_time_ns() - sTimeBaseNs);
}

void osSetTime(OSTime time) {
    sTimeBaseNs = plat_time_ns();
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
