/* Audio interface emulation. Buffers handed to the AI are passed on to the platform's audio output. */
#include <ultra64.h>
#include "types.h"
#include "port.h"

static s32 sAiFrequency = 22050;
static u32 sAiLastBufferSize;
static s32 sAiStarted;

void port_ai_init(void) {
}

s32 osAiSetFrequency(u32 frequency) {
    sAiFrequency = frequency;
    if (!sAiStarted) {
        sAiStarted = TRUE;
        port_audio_output_init(frequency);
    }
    return frequency;
}

s32 osAiSetNextBuffer(void *buf, u32 size) {
    port_audio_output_queue((const s16 *) buf, size);
    sAiLastBufferSize = size;
    return 0;
}

/*
 * Returns how many bytes of the buffer currently being played are left. The hardware only has a two entry
 * FIFO, so anything queued beyond the most recent buffer counts as part of the "current" buffer here.
 */
u32 osAiGetLength(void) {
    u32 remaining = port_audio_output_remaining_bytes();
    if (remaining > sAiLastBufferSize) {
        return remaining - sAiLastBufferSize;
    }
    return 0;
}

u32 osAiGetStatus(void) {
    return 0;
}
