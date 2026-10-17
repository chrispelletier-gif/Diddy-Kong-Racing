/* Temporary host backends. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
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

/* From port.h, which needs the game's headers: the game's clock (fixed steps in DKR_LOCKSTEP runs). */
unsigned long long port_time_ns(void);

static unsigned long long sAudioStart;
static unsigned long long sAudioQueuedBytes;
static unsigned int sAudioFrequency = 22050;

void port_audio_output_init(u32 frequency) {
    sAudioFrequency = frequency;
}

static unsigned long long audio_played_bytes(void) {
    if (sAudioStart == 0) {
        return 0;
    }
    return (port_time_ns() - sAudioStart) * sAudioFrequency / 1000000000ULL * 4;
}

/* DKR_DUMP_AUDIO names a file that receives every buffer as raw 16 bit stereo PCM at the output frequency. */
static void dump_audio(const s16 *samples, u32 numBytes) {
    static FILE *sFile;
    static int sOpened;
    if (!sOpened) {
        const char *path = getenv("DKR_DUMP_AUDIO");
        sOpened = 1;
        if (path != NULL) {
            sFile = fopen(path, "wb");
            if (sFile != NULL) {
                fprintf(stderr, "Dumping audio at %u Hz to %s\n", sAudioFrequency, path);
            }
        }
    }
    if (sFile != NULL) {
        fwrite(samples, 1, numBytes, sFile);
        fflush(sFile);
    }
}

void port_audio_output_queue(const s16 *samples, u32 numBytes) {
    dump_audio(samples, numBytes);
    if (sAudioStart == 0) {
        sAudioStart = port_time_ns();
    }
    if (audio_played_bytes() > sAudioQueuedBytes) {
        // Underrun: restart the clock.
        sAudioStart = port_time_ns();
        sAudioQueuedBytes = 0;
    }
    sAudioQueuedBytes += numBytes;
}

u32 port_audio_output_remaining_bytes(void) {
    unsigned long long played = audio_played_bytes();
    return played >= sAudioQueuedBytes ? 0 : (u32) (sAudioQueuedBytes - played);
}

void port_input_poll(PortControllerState *state, s32 maxControllers) {
    static int sPolls;
    s32 i;
    for (i = 0; i < maxControllers; i++) {
        memset(&state[i], 0, sizeof(state[i]));
    }
    state[0].connected = 1;
    plat_input_script(sPolls++, &state[0].buttons, &state[0].stickX, &state[0].stickY);
}

void port_rumble(s32 controller, s32 enable) {
    (void) controller;
    (void) enable;
}
