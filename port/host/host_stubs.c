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

/*
 * Scripted input for automated testing. DKR_INPUT is a list of "poll:BUTTONS" steps separated by spaces, e.g.
 * "200:START 260: 300:A 330:". BUTTONS is a '+' separated list of A B Z START L R CU CD CL CR DU DD DL DR,
 * or STICK=x,y. Each step lasts until the next one.
 */
static const struct {
    const char *name;
    u16 bit;
} sButtonNames[] = {
    { "A", 0x8000 },  { "B", 0x4000 },  { "Z", 0x2000 },  { "START", 0x1000 }, { "DU", 0x0800 },
    { "DD", 0x0400 }, { "DL", 0x0200 }, { "DR", 0x0100 }, { "L", 0x0020 },     { "R", 0x0010 },
    { "CU", 0x0008 }, { "CD", 0x0004 }, { "CL", 0x0002 }, { "CR", 0x0001 },
};

static void scripted_input(int poll, PortControllerState *state) {
    const char *script = getenv("DKR_INPUT");
    const char *p;
    char token[64];
    int bestPoll = -1;
    char best[64] = "";

    if (script == NULL) {
        return;
    }
    for (p = script; *p;) {
        int n = 0, stepPoll;
        char *colon;
        while (*p == ' ') {
            p++;
        }
        while (*p && *p != ' ' && n < 63) {
            token[n++] = *p++;
        }
        token[n] = 0;
        colon = strchr(token, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = 0;
        stepPoll = atoi(token);
        if (stepPoll <= poll && stepPoll > bestPoll) {
            bestPoll = stepPoll;
            strcpy(best, colon + 1);
        }
    }
    {
        char *tok = strtok(best, "+");
        while (tok != NULL) {
            unsigned i;
            int x, y;
            if (sscanf(tok, "STICK=%d,%d", &x, &y) == 2) {
                state->stickX = (s8) x;
                state->stickY = (s8) y;
            }
            for (i = 0; i < sizeof(sButtonNames) / sizeof(sButtonNames[0]); i++) {
                if (strcmp(tok, sButtonNames[i].name) == 0) {
                    state->buttons |= sButtonNames[i].bit;
                }
            }
            tok = strtok(NULL, "+");
        }
    }
}

void port_input_poll(PortControllerState *state, s32 maxControllers) {
    static int sPolls;
    s32 i;
    for (i = 0; i < maxControllers; i++) {
        memset(&state[i], 0, sizeof(state[i]));
    }
    state[0].connected = 1;
    scripted_input(sPolls++, &state[0]);
}

void port_rumble(s32 controller, s32 enable) {
    (void) controller;
    (void) enable;
}
