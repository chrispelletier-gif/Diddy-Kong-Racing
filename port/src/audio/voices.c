/*
 * The DSP voice sound engine (roadmap Phase 7, the audio rewrite).
 *
 * The game's sequence players and sound player keep running unchanged on the N64 synthesizer driver (libultra's
 * alAudioFrame): they allocate physical voices and queue parameter changes on them (start, stop, volume, pan, pitch,
 * effect mix) with sample-accurate times. Normally the driver then turns those into a command list for the RSP
 * audio microcode, which the port runs on the CPU (audio_rsp.c). Here the command list is never built: each physical
 * voice is one DSP channel, and its parameter changes become channel settings, applied by the DSP's own frame clock
 * (pv_frame_tick, every 160 output samples) so they keep their timing. The DSP decodes, resamples, envelopes and
 * mixes every voice; the samples are decoded from VADPCM to PCM once at start-up.
 *
 * What differs from the N64: the DSP's resampler and its output rate (about 32.7 kHz instead of 22 kHz, no 22 kHz
 * downsampling of the mix), envelope steps every 4.9 ms instead of every 8 samples, and 24 physical voices instead
 * of 40 (the driver steals the quietest as it did on the N64). The reverbs are the N64's own: both aux buses' delay
 * networks run on the DSP's aux returns (pv_aux_process), at the DSP's rate.
 *
 * The CPU mixer stays as the fallback (Settings > Sound > Sound engine, DKR_AUDIO_DSP=0).
 */
#include <ultra64.h>
#include <string.h>
#include "synthInternals.h"
#include "types.h"
#include "port.h"
#include "plat/plat.h"
#include "plat/settings.h"
#include "voices.h"

#define EQPOWER_LENGTH 128
/* The same equal power pan table as env.c. */
static const s16 sEqPower[EQPOWER_LENGTH] = {
    32767, 32764, 32757, 32744, 32727, 32704, 32677, 32644, 32607, 32564, 32517, 32464, 32407, 32344, 32277, 32205,
    32127, 32045, 31958, 31866, 31770, 31668, 31561, 31450, 31334, 31213, 31087, 30957, 30822, 30682, 30537, 30388,
    30234, 30075, 29912, 29744, 29572, 29395, 29214, 29028, 28838, 28643, 28444, 28241, 28033, 27821, 27605, 27385,
    27160, 26931, 26698, 26461, 26220, 25975, 25726, 25473, 25216, 24956, 24691, 24423, 24151, 23875, 23596, 23313,
    23026, 22736, 22442, 22145, 21845, 21541, 21234, 20924, 20610, 20294, 19974, 19651, 19325, 18997, 18665, 18331,
    17993, 17653, 17310, 16965, 16617, 16266, 15913, 15558, 15200, 14840, 14477, 14113, 13746, 13377, 13006, 12633,
    12258, 11881, 11503, 11122, 10740, 10357, 9971,  9584,  9196,  8806,  8415,  8023,  7630,  7235,  6839,  6442,
    6044,  5646,  5246,  4845,  4444,  4042,  3640,  3237,  2833,  2429,  2025,  1620,  1216,  810,   405,   0
};

/******************************** Decoded samples ********************************/

/*
 * A decoded wavetable: the part before the loop (or the whole sample without one), then the loop on its own, as the
 * channels play them. What comes after the loop's end is never heard, so it is not kept.
 */
typedef struct Sample {
    const ALWaveTable *table;
    s16 *head;
    u32 headCount;
    s16 *loop;
    u32 loopCount;
} Sample;

#define SAMPLE_SLOTS 2048 /* a power of two; the banks hold a few hundred wavetables */
static Sample sSamples[SAMPLE_SLOTS];
static u32 sSampleCount;
static u32 sSampleBytes;
static u32 sFiniteLoops;

static s32 sEnabled = -1;
static s32 sActive;
static u32 sSynthRate = 22050;
static PlatSem *sLock;

int pv_enabled(void) {
    if (sEnabled < 0) {
        const char *env = plat_getenv("DKR_AUDIO_DSP");
        sEnabled = (env != NULL) ? (env[0] != '0') : (settings_get("sound_dsp", 1) != 0);
    }
    return sEnabled;
}

int pv_active(void) {
    return sActive;
}

static inline s16 clamp_s16(s32 v) {
    if (v < -0x8000) {
        return -0x8000;
    }
    if (v > 0x7FFF) {
        return 0x7FFF;
    }
    return (s16) v;
}

static Sample *sample_slot(const ALWaveTable *table) {
    u32 i = (((uintptr_t) table) >> 4) & (SAMPLE_SLOTS - 1);
    while (sSamples[i].table != NULL && sSamples[i].table != table) {
        i = (i + 1) & (SAMPLE_SLOTS - 1);
    }
    return &sSamples[i];
}

/* One VADPCM frame (9 bytes) to 16 samples; out[-2] and out[-1] are the previous two samples (audio_rsp.c does the
 * same sums). */
static void adpcm_decode_frame(s16 *out, const u8 *in, const s16 *book) {
    s16 frame[16];
    u32 code = in[0];
    u32 scale = (code & 0xF0) >> 4;
    u32 rshift = (scale < 12) ? 12 - scale : 0;
    const s16 *book1 = book + ((code & 0xF) << 4);
    const s16 *book2 = book1 + 8;
    s32 half, i, k;

    for (i = 0; i < 8; i++) {
        u8 byte = in[1 + i];
        frame[i * 2] = (s16) ((u16) (byte & 0xF0) << 8) >> rshift;
        frame[i * 2 + 1] = (s16) ((u16) (byte & 0x0F) << 12) >> rshift;
    }
    for (half = 0; half < 16; half += 8) {
        const s16 *src = frame + half;
        s16 l1 = out[half - 2];
        s16 l2 = out[half - 1];
        for (i = 0; i < 8; i++) {
            s32 accu = (s32) src[i] << 11;
            accu += book1[i] * l1 + book2[i] * l2;
            for (k = 0; k < i; k++) {
                accu += book2[i - 1 - k] * src[k];
            }
            out[half + i] = clamp_s16(accu >> 11);
        }
    }
}

/* Decodes a wavetable into PCM the channels can play. Returns NULL if it cannot. */
static Sample *sample_decode(ALWaveTable *w) {
    Sample *s = sample_slot(w);
    u32 romOffset = (u32) (uintptr_t) w->base;
    u32 loopStart = 0, loopEnd = 0, loopCount = 0;
    s16 *pcm;
    u32 count, i;

    if (s->table != NULL) {
        return s;
    }
    if (sSampleCount >= SAMPLE_SLOTS / 2) {
        plat_log("voices: too many samples");
        return NULL;
    }
    if (w->type == AL_ADPCM_WAVE) {
        const ALADPCMBook *book = w->waveInfo.adpcmWave.book;
        u32 frames = (u32) w->len / 9;
        const u8 *raw;
        s16 block[2 + 16];
        if (book == NULL || book->order != 2 || book->npredictors < 1 || book->npredictors > 8) {
            plat_log("voices: unsupported ADPCM book (order %d, %d predictors)", book ? (int) book->order : 0,
                     book ? (int) book->npredictors : 0);
            return NULL;
        }
        romOffset &= 0x0FFFFFFF;
        if (romOffset + frames * 9 > port_rom_size()) {
            plat_log("voices: sample outside the ROM");
            return NULL;
        }
        raw = port_rom_data() + romOffset;
        count = frames * 16;
        pcm = pv_backend_alloc(count * 2 + 2);
        if (pcm == NULL) {
            return NULL;
        }
        // The decoder's history starts as zeros, then is the previous frame's last two samples.
        block[0] = 0;
        block[1] = 0;
        for (i = 0; i < frames; i++) {
            adpcm_decode_frame(block + 2, raw + i * 9, book->book);
            memcpy(pcm + i * 16, block + 2, 16 * 2);
            block[0] = block[16];
            block[1] = block[17];
        }
        if (w->waveInfo.adpcmWave.loop != NULL) {
            loopStart = w->waveInfo.adpcmWave.loop->start;
            loopEnd = w->waveInfo.adpcmWave.loop->end;
            loopCount = w->waveInfo.adpcmWave.loop->count;
        }
    } else if (w->type == AL_RAW16_WAVE) {
        count = (u32) w->len / 2;
        pcm = pv_backend_alloc(count * 2 + 2);
        if (pcm == NULL) {
            return NULL;
        }
        port_rom_read(romOffset, pcm, count * 2);
        port_swap16_array(pcm, count);
        if (w->waveInfo.rawWave.loop != NULL) {
            loopStart = w->waveInfo.rawWave.loop->start;
            loopEnd = w->waveInfo.rawWave.loop->end;
            loopCount = w->waveInfo.rawWave.loop->count;
        }
    } else {
        plat_log("voices: unknown wavetable type %d", w->type);
        return NULL;
    }
    if (loopCount != 0 && loopEnd > loopStart && loopStart < count) {
        if (loopEnd > count) {
            loopEnd = count;
        }
        if (loopCount != 0xFFFFFFFF) {
            sFiniteLoops++; // played as endless, like most players do
        }
        s->head = (loopStart > 0) ? pv_backend_alloc(loopStart * 2) : NULL;
        s->headCount = (s->head != NULL) ? loopStart : 0;
        s->loop = pv_backend_alloc((loopEnd - loopStart) * 2);
        if (s->loop == NULL) {
            pv_backend_free(s->head);
            pv_backend_free(pcm);
            return NULL;
        }
        s->loopCount = loopEnd - loopStart;
        if (s->head != NULL) {
            memcpy(s->head, pcm, loopStart * 2);
            pv_backend_commit(s->head, loopStart * 2);
        }
        memcpy(s->loop, pcm + loopStart, s->loopCount * 2);
        pv_backend_commit(s->loop, s->loopCount * 2);
        pv_backend_free(pcm);
        sSampleBytes += (s->headCount + s->loopCount) * 2;
    } else {
        pv_backend_commit(pcm, count * 2);
        s->head = pcm;
        s->headCount = count;
        s->loop = NULL;
        s->loopCount = 0;
        sSampleBytes += count * 2;
    }
    s->table = w;
    sSampleCount++;
    return s;
}

/* Decodes every sample of a bank file when it is loaded (src/audio.c), so no voice waits for its first note. */
void port_voices_prewarm(void *bankFile) {
    ALBankFile *file = bankFile;
    unsigned long long start;
    s32 b, i, j;

    if (!pv_enabled()) {
        return;
    }
    start = plat_time_ns();
    for (b = 0; b < file->bankCount; b++) {
        ALBank *bank = file->bankArray[b];
        if (bank == NULL) {
            continue;
        }
        for (i = -1; i < bank->instCount; i++) {
            ALInstrument *inst = (i < 0) ? bank->percussion : bank->instArray[i];
            if (inst == NULL) {
                continue;
            }
            for (j = 0; j < inst->soundCount; j++) {
                ALSound *sound = inst->soundArray[j];
                if (sound != NULL && sound->wavetable != NULL) {
                    sample_decode(sound->wavetable);
                }
            }
        }
    }
    plat_log("voices: %u samples decoded, %u KB, %llu ms (%u with finite loop counts)", (unsigned) sSampleCount,
             (unsigned) (sSampleBytes / 1024), (plat_time_ns() - start) / 1000000ULL, (unsigned) sFiniteLoops);
}

/******************************** Voice updates from the synthesizer ********************************/

enum { EV_START, EV_START_PLAIN, EV_STOP, EV_VOLUME, EV_PAN, EV_FXMIX, EV_PITCH };

typedef struct Event {
    s64 time; /* in synthesizer samples */
    u8 voice, type, pan, fxMix, unity, bus;
    s16 volume;
    s32 samples; /* attack or ramp length */
    f32 pitch;
    const Sample *sample;
} Event;

#define MAX_EVENTS 512
static Event sEvents[MAX_EVENTS];
static s32 sEventCount;
static u32 sEventsDropped;

static PVoice *sPVoices;
static s32 sNumPVoices;

/* Called from alSynNew with the physical voice array; voice i plays on channel i. */
void port_voices_synth_new(PVoice *pvoices, s32 count) {
    sPVoices = pvoices;
    sNumPVoices = count;
    if (count > PV_CHANNELS) {
        sNumPVoices = PV_CHANNELS;
    }
}

/* src/audio.c sizes the synthesizer with this: one physical voice per DSP channel. */
s32 port_voices_max_pvoices(s32 requested) {
    return pv_enabled() ? PV_CHANNELS : requested;
}

static void post_event(const Event *ev) {
    if (sEventCount == MAX_EVENTS) {
        sEventsDropped++;
        return;
    }
    sEvents[sEventCount++] = *ev;
}

/* Translates one queued parameter change of a voice. */
static void handle_param(ALSynth *drvr, s32 voice, ALParam *p) {
    PVoice *pv = &sPVoices[voice];
    Event ev;

    bzero(&ev, sizeof(ev));
    ev.time = p->delta;
    ev.voice = (u8) voice;
    ev.bus = pv->unkDC & 1;
    switch (p->type) {
        case AL_FILTER_START_VOICE_ALT: {
            ALStartParamAlt *a = (ALStartParamAlt *) p;
            ev.type = EV_START;
            ev.sample = sample_decode(a->wave);
            ev.unity = a->unity != 0;
            ev.pitch = a->pitch;
            ev.volume = a->volume;
            ev.pan = a->pan;
            ev.fxMix = (a->fxMix > 127) ? 127 : a->fxMix;
            ev.samples = a->samples;
            break;
        }
        case AL_FILTER_START_VOICE: {
            ALStartParam *a = (ALStartParam *) p;
            ev.type = EV_START_PLAIN;
            ev.sample = sample_decode(a->wave);
            ev.unity = a->unity != 0;
            break;
        }
        case AL_FILTER_STOP_VOICE:
            ev.type = EV_STOP;
            break;
        case AL_FILTER_SET_VOLUME:
            ev.type = EV_VOLUME;
            ev.volume = (s16) p->data.i;
            ev.samples = p->moredata.i;
            break;
        case AL_FILTER_SET_PAN:
            ev.type = EV_PAN;
            ev.pan = (u8) (p->data.i & 127);
            break;
        case AL_FILTER_SET_FXAMT:
            ev.type = EV_FXMIX;
            ev.fxMix = (u8) ((p->data.i < 0 ? -p->data.i : p->data.i) & 127);
            break;
        case AL_FILTER_SET_PITCH:
            ev.type = EV_PITCH;
            ev.pitch = p->data.f;
            break;
        case AL_FILTER_FREE_VOICE: {
            ALFreeParam *f = (ALFreeParam *) p;
            f->pvoice->offset = 0;
            _freePVoice(drvr, (PVoice *) f->pvoice);
            return;
        }
        default:
            return;
    }
    post_event(&ev);
}

/*
 * Replaces the command list building of alAudioFrame: the parameter changes due within this frame leave the voices'
 * queues for the channels. Returns 0 when the engine is not running and the command list must be built after all.
 */
s32 port_voices_frame(ALSynth *drvr, s32 outLen) {
    s32 frameEnd = drvr->curSamples + outLen;
    s32 i;

    if (!sActive || sPVoices == NULL) {
        return FALSE;
    }
    plat_sem_wait(sLock);
    for (i = 0; i < sNumPVoices; i++) {
        ALEnvMixer *e = &sPVoices[i].envmixer;
        while (e->ctrlList != NULL && e->ctrlList->delta <= frameEnd) {
            ALParam *p = e->ctrlList;
            handle_param(drvr, i, p);
            e->ctrlList = p->next;
            if (e->ctrlList == NULL) {
                e->ctrlTail = NULL;
            }
            __freeParam(p);
        }
    }
    plat_sem_post(sLock);
    return TRUE;
}

/******************************** The channels, driven by the DSP's frame clock ********************************/

typedef struct Voice {
    s32 playing;
    const Sample *sample;
    s16 volume, pan, dry, wet;
    s32 bus;
    f32 ratio;
    s32 unity;
    f32 curL, curR, tgtL, tgtR; /* envelope gains, in the N64's 0..32767 units */
    f32 rampLeft;               /* synthesizer samples until the target is reached */
    f32 mix[PV_MIX_COUNT];
} Voice;

static Voice sVoices[PV_CHANNELS];
static s32 sPeakPlaying;
/* DKR_AUDIO_DEBUG=1 logs these now and then: how late events were applied (0 to one DSP frame is on time). */
static s32 sDebug = -1;
static double sLateSum, sLateMax;
static u32 sLateCount;
static u32 sAuxFrames[2];
static s32 sNoFx = -1;

/* The output clock. Playback starts when the game queues its first buffer. */
static s32 sClockStarted;
static u32 sFrame0;
static unsigned long long sQueuedBytes;

static double played_samples(void) {
    if (!sClockStarted) {
        return 0;
    }
    return (double) (pv_backend_frames() - sFrame0) * PV_FRAME_SAMPLES * sSynthRate / PV_OUTPUT_RATE;
}

/*
 * When the game stops queueing audio (it froze, crashed or is paused), the channels would go on looping their
 * samples: with no new output a CPU mixer falls silent, so the voices are muted after this many frames without a
 * buffer (about 0.3 s), and unmuted when the game queues again.
 */
#define PV_STALL_FRAMES 64
static u32 sLastQueueFrame;
static s32 sMuted;

void pv_output_queue(unsigned numBytes) {
    if (!sClockStarted) {
        sFrame0 = pv_backend_frames();
        sClockStarted = TRUE;
    }
    sLastQueueFrame = pv_backend_frames();
    sQueuedBytes += numBytes;
}

unsigned pv_output_remaining(void) {
    double played = played_samples() * 4;
    return (played >= (double) sQueuedBytes) ? 0 : (unsigned) ((double) sQueuedBytes - played);
}

static void voice_targets(Voice *v) {
    v->tgtL = (f32) ((v->volume * sEqPower[v->pan]) >> 15);
    v->tgtR = (f32) ((v->volume * sEqPower[EQPOWER_LENGTH - v->pan - 1]) >> 15);
}

static void voice_mix(Voice *v) {
    f32 l = v->curL / 32768.0f;
    f32 r = v->curR / 32768.0f;
    s32 aux = (v->bus == 1) ? PV_MIX_AUX1 : PV_MIX_AUX0;
    bzero(v->mix, sizeof(v->mix));
    v->mix[PV_MIX_FRONT_LEFT] = l * (v->dry / 32768.0f);
    v->mix[PV_MIX_FRONT_RIGHT] = r * (v->dry / 32768.0f);
    v->mix[aux] = l * (v->wet / 32768.0f);
    v->mix[aux + 1] = r * (v->wet / 32768.0f);
}

static void voice_start(Voice *v, s32 ch) {
    voice_mix(v);
    if (v->sample == NULL || v->sample->headCount + v->sample->loopCount == 0) {
        v->playing = FALSE;
        pv_backend_stop(ch);
        return;
    }
    v->playing = TRUE;
    pv_backend_play(ch, v->sample->head, v->sample->headCount, v->sample->loop, v->sample->loopCount,
                    v->ratio * (f32) sSynthRate, v->mix);
}

/*
 * After the console comes back from the HOME menu or sleep, the DSP has been unloaded and reloaded. Whether a channel
 * went on with its sample after that is up to the firmware and libctru's bookkeeping of it; the voices that were
 * playing are restarted from a known point instead: a looping sample from its loop (a held note or an engine goes
 * on sounding), a one-shot sample is dropped (it would have ended anyway). Called by the backend from its frame
 * thread once the DSP is back.
 */
static u32 sResyncs;

void pv_resync(void) {
    s32 i, restarted = 0, dropped = 0;
    for (i = 0; i < PV_CHANNELS; i++) {
        Voice *v = &sVoices[i];
        if (!v->playing || v->sample == NULL) {
            continue;
        }
        if (v->sample->loopCount != 0) {
            static const f32 sSilence[PV_MIX_COUNT];
            pv_backend_play(i, NULL, 0, v->sample->loop, v->sample->loopCount, v->ratio * (f32) sSynthRate,
                            sMuted ? sSilence : v->mix);
            restarted++;
        } else {
            v->playing = FALSE;
            pv_backend_stop(i);
            dropped++;
        }
    }
    sResyncs++;
    plat_log("voices: resumed, %d looping voices restarted, %d one-shot voices dropped", (int) restarted,
             (int) dropped);
}

static void apply_event(const Event *ev) {
    Voice *v = &sVoices[ev->voice];
    s32 ch = ev->voice;

    switch (ev->type) {
        case EV_START:
            v->sample = ev->sample;
            v->bus = ev->bus;
            v->volume = ev->volume;
            v->pan = ev->pan;
            v->dry = sEqPower[ev->fxMix];
            v->wet = sEqPower[EQPOWER_LENGTH - ev->fxMix - 1];
            v->unity = ev->unity;
            v->ratio = ev->unity ? 1.0f : ev->pitch;
            voice_targets(v);
            if (ev->samples > 0) {
                v->curL = 1.0f;
                v->curR = 1.0f;
                v->rampLeft = (f32) ev->samples;
            } else {
                v->curL = v->tgtL;
                v->curR = v->tgtR;
                v->rampLeft = 0;
            }
            voice_start(v, ch);
            break;
        case EV_START_PLAIN:
            v->sample = ev->sample;
            v->bus = ev->bus;
            v->unity = ev->unity;
            if (ev->unity) {
                v->ratio = 1.0f;
            }
            voice_start(v, ch);
            break;
        case EV_STOP:
            v->playing = FALSE;
            // The N64 filter resets to a volume of 1: a plain restart is silent until the next volume change.
            v->volume = 1;
            voice_targets(v);
            v->curL = v->tgtL;
            v->curR = v->tgtR;
            v->rampLeft = 0;
            pv_backend_stop(ch);
            break;
        case EV_VOLUME:
            v->volume = ev->volume;
            voice_targets(v);
            v->rampLeft = (f32) ev->samples;
            if (ev->samples <= 0) {
                v->curL = v->tgtL;
                v->curR = v->tgtR;
            }
            break;
        case EV_PAN:
            v->pan = ev->pan;
            voice_targets(v);
            if (v->rampLeft <= 0) {
                v->curL = v->tgtL;
                v->curR = v->tgtR;
            }
            break;
        case EV_FXMIX:
            v->dry = sEqPower[ev->fxMix];
            v->wet = sEqPower[EQPOWER_LENGTH - ev->fxMix - 1];
            break;
        case EV_PITCH:
            if (!v->unity) {
                v->ratio = ev->pitch;
                if (v->playing) {
                    pv_backend_set_rate(ch, v->ratio * (f32) sSynthRate);
                }
            }
            break;
    }
}

void pv_frame_tick(void) {
    f32 step = (f32) PV_FRAME_SAMPLES * sSynthRate / PV_OUTPUT_RATE;
    double now;
    s32 i, k, n, playing = 0;

    if (!sActive) {
        return;
    }
    plat_sem_wait(sLock);
    now = played_samples();
    // Events whose time has come, in the order they were queued; later ones wait (they stay in order).
    n = 0;
    for (i = 0; i < sEventCount; i++) {
        if ((double) sEvents[i].time <= now) {
            double late = now - (double) sEvents[i].time;
            sLateSum += late;
            sLateCount++;
            if (late > sLateMax) {
                sLateMax = late;
            }
            apply_event(&sEvents[i]);
        } else {
            sEvents[n++] = sEvents[i];
        }
    }
    sEventCount = n;
    plat_sem_post(sLock);

    if (sClockStarted) {
        s32 stalled = (pv_backend_frames() - sLastQueueFrame) > PV_STALL_FRAMES;
        if (stalled != sMuted) {
            static const f32 sSilence[PV_MIX_COUNT];
            sMuted = stalled;
            for (i = 0; i < PV_CHANNELS; i++) {
                if (sVoices[i].playing) {
                    pv_backend_set_mix(i, stalled ? sSilence : sVoices[i].mix);
                }
            }
            if (sDebug > 0) {
                plat_log("voices: %s, the game %s queueing audio", stalled ? "muted" : "unmuted",
                         stalled ? "stopped" : "resumed");
            }
        }
    }

    for (i = 0; i < PV_CHANNELS; i++) {
        Voice *v = &sVoices[i];
        f32 mixBefore[PV_MIX_COUNT];
        if (!v->playing) {
            continue;
        }
        playing++;
        memcpy(mixBefore, v->mix, sizeof(mixBefore));
        if (v->rampLeft > 0) {
            f32 f = (step >= v->rampLeft) ? 1.0f : step / v->rampLeft;
            v->curL += (v->tgtL - v->curL) * f;
            v->curR += (v->tgtR - v->curR) * f;
            v->rampLeft -= step;
            if (v->rampLeft <= 0) {
                v->rampLeft = 0;
                v->curL = v->tgtL;
                v->curR = v->tgtR;
            }
        }
        voice_mix(v);
        for (k = 0; k < PV_MIX_COUNT && !sMuted; k++) {
            if (mixBefore[k] != v->mix[k]) {
                pv_backend_set_mix(i, v->mix);
                break;
            }
        }
    }
    if (playing > sPeakPlaying) {
        sPeakPlaying = playing;
    }
    if (sDebug < 0) {
        const char *env = plat_getenv("DKR_AUDIO_DEBUG");
        sDebug = (env != NULL && env[0] == '1');
    }
    if (sDebug && (pv_backend_frames() % 1024) == 0 && sLateCount != 0) {
        plat_log("voices: %d playing (peak %d), %u events late by %.0f avg %.0f max samples, %u dropped, "
                 "%u pending, aux frames %u/%u",
                 (int) playing, (int) sPeakPlaying, (unsigned) sLateCount, sLateSum / sLateCount, sLateMax,
                 (unsigned) sEventsDropped, (unsigned) sEventCount, (unsigned) sAuxFrames[0],
                 (unsigned) sAuxFrames[1]);
    }
}

const char *pv_describe(void) {
    static char sText[32];
    sprintf(sText, "dsp/%dv", (int) sPeakPlaying);
    return sText;
}

/* For bug reports: the engine's state and counters in one line. */
const char *pv_status(void) {
    static char sText[160];
    s32 i, playing = 0;
    for (i = 0; i < PV_CHANNELS; i++) {
        playing += sVoices[i].playing ? 1 : 0;
    }
    sprintf(sText, "%d playing (peak %d), %u events late by %.0f avg %.0f max samples, %u dropped, %u pending, "
                   "%s, %u resumes, aux frames %u/%u",
            (int) playing, (int) sPeakPlaying, (unsigned) sLateCount, sLateCount ? sLateSum / sLateCount : 0.0,
            sLateMax, (unsigned) sEventsDropped, (unsigned) sEventCount, sMuted ? "muted" : "unmuted",
            (unsigned) sResyncs, (unsigned) sAuxFrames[0], (unsigned) sAuxFrames[1]);
    return sText;
}

/******************************** The N64 reverb on the aux returns ********************************/

/*
 * alFxPull (reverb.c) as sample arithmetic instead of RSP commands, on the same ALFx delay line and sections that
 * alFxNew set up (their lengths scaled to the DSP's rate by port_fx_rate_scale). Each section reads the line at
 * two taps, feeds forward and back, optionally modulates (chorus) and low passes, and adds to the output; the
 * output goes to both returns. The RSP's saturating 16 bit arithmetic is kept so the sound matches.
 */
#define FX_MAX 256

static inline s16 mix_q15(s16 dst, s32 src, s16 gain) {
    return clamp_s16(dst + ((src * gain) >> 15));
}

static s16 *ring_ptr(ALFx *r, s16 *ptr) {
    if (ptr < r->base) {
        ptr += r->length;
    }
    return ptr;
}

static void ring_load(ALFx *r, s16 *from, s16 *buf, s32 count) {
    s16 *end = &r->base[r->length];
    s32 i;
    from = ring_ptr(r, from);
    for (i = 0; i < count; i++) {
        if (from >= end) {
            from -= r->length;
        }
        buf[i] = *from++;
    }
}

static void ring_save(ALFx *r, s16 *to, const s16 *buf, s32 count) {
    s16 *end = &r->base[r->length];
    s32 i;
    to = ring_ptr(r, to);
    for (i = 0; i < count; i++) {
        if (to >= end) {
            to -= r->length;
        }
        *to++ = buf[i];
    }
}

/* reverb.c's _doModFunc: a triangle wave scaled by the chorus depth. rsinc is per sample at the game's rate. */
static f32 mod_func(ALDelay *d, s32 count, f32 rateScale) {
    f32 val;
    d->rsval += d->rsinc * rateScale * count;
    d->rsval = (d->rsval > 2.0f) ? d->rsval - 4.0f : d->rsval;
    val = (d->rsval < 0) ? -d->rsval : d->rsval;
    val -= 1.0f;
    return d->rsgain * val;
}

static void load_output_buffer(ALFx *r, ALDelay *d, s16 *buf, s32 count, f32 rateScale) {
    if (d->rs != NULL) {
        s16 src[FX_MAX * 2 + 4];
        s32 length = d->output - d->input;
        f32 delta = mod_func(d, count, rateScale) / (f32) length;
        f32 fratio, fincount, pos;
        s32 n, i;
        delta = (f32) ((s32) (delta * 32768.0f)) / 32768.0f;
        fratio = 1.0f - delta;
        fincount = d->rs->delta + fratio * (f32) count;
        n = (s32) fincount;
        d->rs->delta = fincount - (f32) n;
        if (n > FX_MAX * 2) {
            n = FX_MAX * 2;
        }
        ring_load(r, &r->input[-(d->output - d->rsdelta)], src, n + 2);
        pos = 0;
        for (i = 0; i < count; i++) {
            s32 k = (s32) pos;
            f32 frac = pos - (f32) k;
            buf[i] = (s16) (src[k] + (src[k + 1] - src[k]) * frac);
            pos += fratio;
        }
        d->rsdelta += n - count;
    } else {
        ring_load(r, &r->input[-d->output], buf, count);
    }
}

/* A one pole low pass (aPoleFilter with the coefficients _init_lpfilter builds): y = (gain x + fc y') / 16384. */
static void low_pass(ALLowPass *lp, s16 *buf, s32 count) {
    s16 *state = (s16 *) lp->fstate;
    s32 prev = lp->first ? 0 : state[3];
    s32 fc = lp->fcvec.fccoef[8];
    s32 i;
    lp->first = 0;
    for (i = 0; i < count; i++) {
        prev = clamp_s16((buf[i] * lp->fgain + prev * fc) >> 14);
        buf[i] = (s16) prev;
    }
    state[3] = (s16) prev;
}

extern u8 alFXEnabled;

static void fx_process(ALFx *r, int *left, int *right, s32 count, f32 rateScale) {
    s16 input[FX_MAX], output[FX_MAX], bufA[FX_MAX], bufB[FX_MAX];
    s16 *buff1 = bufA, *buff2 = bufB;
    s16 *prevOut = NULL;
    s32 i, s;

    if (!alFXEnabled || r->section_count == 0 || r->length == 0) {
        for (i = 0; i < count; i++) {
            left[i] = right[i] = 0;
        }
        return;
    }
    // .707 L + .707 R into the delay line, as the RSP's two mixes (0xda83 then 0x5a82) do.
    for (i = 0; i < count; i++) {
        s16 l = clamp_s16(left[i]);
        s16 rr = clamp_s16(right[i]);
        input[i] = mix_q15(mix_q15(l, l, (s16) 0xDA83), rr, 0x5A82);
        output[i] = 0;
    }
    ring_save(r, r->input, input, count);

    for (s = 0; s < r->section_count; s++) {
        ALDelay *d = &r->delay[s];
        s16 *inPtr = &r->input[-(s32) d->input];
        s16 *outPtr = &r->input[-(s32) d->output];
        if (inPtr == prevOut) {
            s16 *t = buff1;
            buff1 = buff2;
            buff2 = t;
        } else {
            ring_load(r, inPtr, buff1, count);
        }
        load_output_buffer(r, d, buff2, count, rateScale);
        if (d->ffcoef) {
            for (i = 0; i < count; i++) {
                buff2[i] = mix_q15(buff2[i], buff1[i], d->ffcoef);
            }
            if (d->rs == NULL && d->lp == NULL) {
                ring_save(r, outPtr, buff2, count);
            }
        }
        if (d->fbcoef) {
            for (i = 0; i < count; i++) {
                buff1[i] = mix_q15(buff1[i], buff2[i], d->fbcoef);
            }
            ring_save(r, inPtr, buff1, count);
        }
        if (d->lp != NULL) {
            low_pass(d->lp, buff2, count);
        }
        if (d->rs == NULL) {
            ring_save(r, outPtr, buff2, count);
        }
        if (d->gain) {
            for (i = 0; i < count; i++) {
                output[i] = mix_q15(output[i], buff2[i], d->gain);
            }
        }
        prevOut = &r->input[d->output];
    }
    r->input += count;
    if (r->input > &r->base[r->length]) {
        r->input -= r->length;
    }
    for (i = 0; i < count; i++) {
        left[i] = right[i] = output[i];
    }
}

/* Delay lengths in drvrnew.c are in samples at the game's rate; here the reverb runs at the DSP's. */
f32 port_fx_rate_scale(void) {
    return pv_enabled() ? (f32) sSynthRate / PV_OUTPUT_RATE : gPortAudioRateScale;
}

void pv_aux_process(int bus, int *left, int *right, int count) {
    ALSynth *drvr;
    s32 done = 0;

    if (!sActive || alGlobals == NULL || bus < 0 || bus > 1) {
        return;
    }
    drvr = &alGlobals->drvr;
    if (sNoFx < 0) {
        const char *env = plat_getenv("DKR_AUDIO_NOFX");
        sNoFx = (env != NULL && env[0] == '1');
    }
    for (done = 0; done < count; done++) {
        if (left[done] != 0 || right[done] != 0) {
            sAuxFrames[bus]++;
            break;
        }
    }
    if (sNoFx) {
        // For comparison: no reverb at all.
        bzero(left, count * sizeof(int));
        bzero(right, count * sizeof(int));
        return;
    }
    done = 0;
    while (done < count) {
        s32 n = (count - done > FX_MAX) ? FX_MAX : count - done;
        fx_process(&drvr->auxBus[bus].fx[0], left + done, right + done, n, port_fx_rate_scale());
        done += n;
    }
}

/******************************** Start-up ********************************/

/* From the platform's audio output start (osAiSetFrequency): the backend's channels, if the player wants them. */
void port_voices_start(u32 synthRate) {
    if (!pv_enabled()) {
        return;
    }
    sSynthRate = synthRate;
    if (sLock == NULL) {
        sLock = plat_sem_create(1);
    }
    if (!pv_backend_init(synthRate)) {
        plat_log("voices: no DSP channels, using the CPU mixer");
        return;
    }
    sActive = TRUE;
    plat_log("voices: DSP voices at %u Hz, %u samples decoded (%u KB)", (unsigned) synthRate,
             (unsigned) sSampleCount, (unsigned) (sSampleBytes / 1024));
}
