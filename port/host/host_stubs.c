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
void port_voices_start(u32 synthRate);

static unsigned long long sAudioStart;
static unsigned long long sAudioQueuedBytes;
static unsigned int sAudioFrequency = 22050;

#include "audio/voices.h"
void host_soft_audio_queue(unsigned numBytes);

const char *plat_audio_setup(void) {
    return pv_active() ? pv_describe() : "host";
}

int plat_renderer_syscore_share(void) {
    return 0;
}

/* DKR_AUDIO_RATE runs the mixer at another rate (32000 for the 3DS's "High" sound quality). */
u32 port_audio_output_rate(u32 requested) {
    const char *env = getenv("DKR_AUDIO_RATE");
    return (env != NULL && atoi(env) > 0) ? (u32) atoi(env) : requested;
}

void port_audio_output_init(u32 frequency) {
    sAudioFrequency = frequency;
    port_voices_start(frequency);
}

static unsigned long long audio_played_bytes(void) {
    if (sAudioStart == 0) {
        return 0;
    }
    return (port_time_ns() - sAudioStart) * sAudioFrequency / 1000000000ULL * 4;
}

/* DKR_DUMP_AUDIO names a file that receives the output as raw 16 bit stereo PCM at the output frequency. */
static FILE *sDumpFile;

void host_audio_dump_open(unsigned rate) {
    static int sOpened;
    if (!sOpened) {
        const char *path = getenv("DKR_DUMP_AUDIO");
        sOpened = 1;
        if (path != NULL) {
            sDumpFile = fopen(path, "wb");
            if (sDumpFile != NULL) {
                fprintf(stderr, "Dumping audio at %u Hz to %s\n", rate, path);
            }
        }
    }
}

void host_audio_dump_write(const void *data, unsigned numBytes) {
    if (sDumpFile != NULL) {
        fwrite(data, 1, numBytes, sDumpFile);
        fflush(sDumpFile);
    }
}

void port_audio_output_queue(const s16 *samples, u32 numBytes) {
    if (pv_active()) {
        // The DSP voices: the software channels play instead of this buffer.
        host_soft_audio_queue(numBytes);
        return;
    }
    host_audio_dump_open(sAudioFrequency);
    host_audio_dump_write(samples, numBytes);
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
    if (pv_active()) {
        return pv_output_remaining();
    }
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
