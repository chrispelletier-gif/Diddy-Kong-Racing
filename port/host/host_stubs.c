/* Temporary host backends. */
#include <stdlib.h>
#include <string.h>
#include "plat/plat.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef short s16;
typedef int s32;

typedef struct PortControllerState {
    u16 buttons;
    s8 stickX;
    s8 stickY;
    u8 connected;
} PortControllerState;

static volatile int sFrames;
static unsigned long long sAudioStart;
static unsigned long long sAudioQueuedBytes;
static unsigned int sAudioFrequency = 22050;

int host_frames_rendered(void) {
    return sFrames;
}

void port_gfx_init(void) {
}

void port_gfx_run_task(void *task) {
    (void) task;
    sFrames++;
}

void port_audio_run_task(void *task) {
    (void) task;
}

void port_audio_output_init(u32 frequency) {
    sAudioFrequency = frequency;
}

static unsigned long long audio_played_bytes(void) {
    if (sAudioStart == 0) {
        return 0;
    }
    return (plat_time_ns() - sAudioStart) * sAudioFrequency / 1000000000ULL * 4;
}

void port_audio_output_queue(const s16 *samples, u32 numBytes) {
    (void) samples;
    if (sAudioStart == 0) {
        sAudioStart = plat_time_ns();
    }
    if (audio_played_bytes() > sAudioQueuedBytes) {
        // Underrun: restart the clock.
        sAudioStart = plat_time_ns();
        sAudioQueuedBytes = 0;
    }
    sAudioQueuedBytes += numBytes;
}

u32 port_audio_output_remaining_bytes(void) {
    unsigned long long played = audio_played_bytes();
    return played >= sAudioQueuedBytes ? 0 : (u32) (sAudioQueuedBytes - played);
}

void port_input_poll(PortControllerState *state, s32 maxControllers) {
    s32 i;
    for (i = 0; i < maxControllers; i++) {
        memset(&state[i], 0, sizeof(state[i]));
    }
    state[0].connected = 1;
}

void port_rumble(s32 controller, s32 enable) {
    (void) controller;
    (void) enable;
}
