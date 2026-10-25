/*
 * A software stand-in for the 3DS DSP's channels, for the host build (port/src/audio/voices.h). It mixes the same
 * way: 24 channels of mono 16 bit PCM, each at its own rate (linear interpolation) and with 12 mixing volumes into
 * the main output and two aux buses, which pv_aux_process (the N64 reverb) turns into returns, in frames of 160
 * samples at the DSP's rate. There is no sound device: the frames are mixed whenever the game queues audio, so a
 * DKR_DUMP_AUDIO recording of them is deterministic.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "plat/plat.h"
#include "audio/voices.h"

typedef short s16;
typedef int s32;
typedef unsigned int u32;

typedef struct SoftChannel {
    int playing;
    const s16 *head, *loop;
    u32 headCount, loopCount;
    int inLoop;
    double pos;
    float rate;
    float mix[PV_MIX_COUNT];
} SoftChannel;

static SoftChannel sChannels[PV_CHANNELS];
static unsigned sFrames;
static unsigned sSynthRate = 22050;
static unsigned long long sQueuedSamples; /* synthesizer samples the game has queued */

void host_audio_dump_open(unsigned rate);
void host_audio_dump_write(const void *data, unsigned bytes);

int pv_backend_init(unsigned synthRate) {
    sSynthRate = synthRate;
    return 1;
}

void *pv_backend_alloc(unsigned bytes) {
    return calloc(1, bytes);
}

void pv_backend_free(void *data) {
    free(data);
}

void pv_backend_commit(const void *data, unsigned bytes) {
    (void) data;
    (void) bytes;
}

void pv_backend_play(int channel, const s16 *head, unsigned headSamples, const s16 *loop, unsigned loopSamples,
                     float rate, const float mix[PV_MIX_COUNT]) {
    SoftChannel *c = &sChannels[channel];
    c->head = head;
    c->headCount = headSamples;
    c->loop = loop;
    c->loopCount = loopSamples;
    c->inLoop = (headSamples == 0);
    c->pos = 0;
    c->rate = rate;
    memcpy(c->mix, mix, sizeof(c->mix));
    c->playing = (headSamples + loopSamples) != 0;
}

void pv_backend_stop(int channel) {
    sChannels[channel].playing = 0;
}

void pv_backend_set_rate(int channel, float rate) {
    sChannels[channel].rate = rate;
}

void pv_backend_set_mix(int channel, const float mix[PV_MIX_COUNT]) {
    memcpy(sChannels[channel].mix, mix, sizeof(sChannels[channel].mix));
}

unsigned pv_backend_frames(void) {
    return sFrames;
}

static inline s16 clamp_s16(s32 v) {
    return (s16) (v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v);
}

/* One frame: the channels into the main mix and the aux buses, the reverbs, then the output. */
static void mix_frame(void) {
    static s32 main[PV_FRAME_SAMPLES][2];
    static s32 aux[2][2][PV_FRAME_SAMPLES];
    static s16 out[PV_FRAME_SAMPLES * 2];
    int ch, i, bus;

    pv_frame_tick();
    memset(main, 0, sizeof(main));
    memset(aux, 0, sizeof(aux));
    for (ch = 0; ch < PV_CHANNELS; ch++) {
        SoftChannel *c = &sChannels[ch];
        double step;
        if (!c->playing) {
            continue;
        }
        step = c->rate / PV_OUTPUT_RATE;
        for (i = 0; i < PV_FRAME_SAMPLES; i++) {
            const s16 *pcm = c->inLoop ? c->loop : c->head;
            u32 count = c->inLoop ? c->loopCount : c->headCount;
            u32 k = (u32) c->pos;
            float frac, sample, next;
            if (k >= count) {
                // The end of the head goes on into the loop; the loop's end into its start.
                if (c->inLoop) {
                    c->pos -= (double) count;
                } else if (c->loopCount != 0) {
                    c->pos -= (double) count;
                    c->inLoop = 1;
                } else {
                    c->playing = 0;
                    break;
                }
                pcm = c->inLoop ? c->loop : c->head;
                count = c->inLoop ? c->loopCount : c->headCount;
                k = (u32) c->pos;
                if (k >= count) {
                    k = 0;
                    c->pos = 0;
                }
            }
            frac = (float) (c->pos - (double) k);
            if (k + 1 < count) {
                next = pcm[k + 1];
            } else if (c->loopCount != 0) {
                next = c->loop[0];
            } else {
                next = pcm[k];
            }
            sample = pcm[k] + (next - pcm[k]) * frac;
            main[i][0] += (s32) (sample * c->mix[PV_MIX_FRONT_LEFT]);
            main[i][1] += (s32) (sample * c->mix[PV_MIX_FRONT_RIGHT]);
            aux[0][0][i] += (s32) (sample * c->mix[PV_MIX_AUX0]);
            aux[0][1][i] += (s32) (sample * c->mix[PV_MIX_AUX0 + 1]);
            aux[1][0][i] += (s32) (sample * c->mix[PV_MIX_AUX1]);
            aux[1][1][i] += (s32) (sample * c->mix[PV_MIX_AUX1 + 1]);
            c->pos += step;
        }
    }
    for (bus = 0; bus < 2; bus++) {
        pv_aux_process(bus, aux[bus][0], aux[bus][1], PV_FRAME_SAMPLES);
        for (i = 0; i < PV_FRAME_SAMPLES; i++) {
            main[i][0] += aux[bus][0][i];
            main[i][1] += aux[bus][1][i];
        }
    }
    for (i = 0; i < PV_FRAME_SAMPLES; i++) {
        out[i * 2] = clamp_s16(main[i][0]);
        out[i * 2 + 1] = clamp_s16(main[i][1]);
    }
    sFrames++;
    host_audio_dump_write(out, sizeof(out));
}

/* The game queued numBytes of (unused) mixer output: mix the frames that fit in the time it covers. */
void host_soft_audio_queue(unsigned numBytes) {
    if (sFrames == 0) {
        host_audio_dump_open((unsigned) PV_OUTPUT_RATE);
    }
    pv_output_queue(numBytes);
    sQueuedSamples += numBytes / 4;
    // Stay a buffer and a little behind the game, as a real output does: that is what its frame sizing settles at.
    while ((double) (sFrames + 1) * PV_FRAME_SAMPLES * sSynthRate / PV_OUTPUT_RATE + numBytes / 4 + 112 <=
           (double) sQueuedSamples) {
        mix_frame();
    }
}
