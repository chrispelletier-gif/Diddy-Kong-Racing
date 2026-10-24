/* Audio interface emulation. Buffers handed to the AI are passed on to the platform's audio output. */
#include <ultra64.h>
#include "types.h"
#include "port.h"

static s32 sAiFrequency = 22050;
static u32 sAiLastBufferSize;
static s32 sAiStarted;
float gPortAudioRateScale = 1.0f;

void port_ai_init(void) {
}

s32 osAiSetFrequency(u32 frequency) {
    if (!sAiStarted) {
        u32 rate = port_audio_output_rate(frequency);
        sAiStarted = TRUE;
        gPortAudioRateScale = (float) frequency / (float) rate;
        frequency = rate;
        port_audio_output_init(frequency);
    } else {
        frequency = sAiFrequency;
    }
    sAiFrequency = frequency;
    return frequency;
}

s32 osAiSetNextBuffer(void *buf, u32 size) {
    port_audio_output_queue((const s16 *) buf, size);
    sAiLastBufferSize = size;
    return 0;
}

/*
 * Returns how many bytes of the buffer currently being played are left. The hardware has a two entry FIFO, so
 * the value never exceeds the size of one buffer. The audio manager sizes the next frame from this, so keeping it
 * in range keeps production matched to playback even when the output device buffers more than the N64 would.
 */
u32 osAiGetLength(void) {
    u32 remaining = port_audio_output_remaining_bytes();
    if (remaining <= sAiLastBufferSize) {
        return 0;
    }
    remaining -= sAiLastBufferSize;
    return (remaining > sAiLastBufferSize) ? sAiLastBufferSize : remaining;
}

u32 osAiGetStatus(void) {
    return 0;
}
