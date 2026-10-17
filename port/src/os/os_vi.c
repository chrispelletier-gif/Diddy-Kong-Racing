/*
 * Video interface emulation. The N64 framebuffer is never scanned out (the renderer draws straight to the
 * screen), but the game relies on the retrace interrupt for timing and on the framebuffer swap bookkeeping.
 */
#include <ultra64.h>
#include "types.h"
#include "port.h"
#include "plat/plat.h"

static OSMesgQueue *sViEventQueue;
static OSMesg sViEventMsg;
static u32 sViRetraceCount = 1;
static u32 sViRetraceCounter;
static void *sViCurrentFramebuffer;
static void *sViNextFramebuffer;
static OSViMode *sViMode;
static u8 sViBlack;
static s32 sViStarted;

static void vi_thread(void *arg) {
    (void) arg;
    port_int_thread_init();
    for (;;) {
        plat_wait_vblank();
        // Interrupts are not taken while a thread has them disabled.
        plat_intlock_lock();
        sViCurrentFramebuffer = sViNextFramebuffer;
        sViRetraceCounter++;
        gPlatCounters[PLAT_COUNTER_VI]++;
        if (sViEventQueue != NULL && sViRetraceCounter >= sViRetraceCount) {
            sViRetraceCounter = 0;
            port_post_mesg(sViEventQueue, sViEventMsg);
        }
        plat_intlock_unlock();
    }
}

void port_vi_init(void) {
    if (!sViStarted) {
        sViStarted = TRUE;
        plat_thread_create(vi_thread, NULL, 0, PLAT_THREAD_HARDWARE, "vi");
    }
}

void osCreateViManager(OSPri pri) {
    (void) pri;
    port_vi_init();
}

void osViSetEvent(OSMesgQueue *mq, OSMesg msg, u32 retraceCount) {
    sViEventQueue = mq;
    sViEventMsg = msg;
    sViRetraceCount = (retraceCount == 0) ? 1 : retraceCount;
}

void osViSetMode(OSViMode *mode) {
    sViMode = mode;
}

u32 osViGetCurrentMode(void) {
    return (sViMode != NULL) ? sViMode->type : 0;
}

void osViSetSpecialFeatures(u32 func) {
    (void) func;
}

void osViSetXScale(f32 scale) {
    (void) scale;
}

void osViSetYScale(f32 scale) {
    (void) scale;
}

void osViBlack(u8 active) {
    sViBlack = active;
}

u8 port_vi_is_black(void) {
    return sViBlack;
}

void osViSwapBuffer(void *frameBufPtr) {
    sViNextFramebuffer = frameBufPtr;
}

void *osViGetCurrentFramebuffer(void) {
    return sViCurrentFramebuffer;
}

void *osViGetNextFramebuffer(void) {
    return sViNextFramebuffer;
}
