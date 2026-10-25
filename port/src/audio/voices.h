/*
 * The DSP voice sound engine (roadmap Phase 7, the audio rewrite): the interface between the game side
 * (src/audio/voices.c, which turns the N64 synthesizer's voice updates into channel settings) and the platform
 * backend that owns the channels (3ds/audio_ndsp.c on the DSP, host/audio_soft.c as a software stand-in).
 * Only builtin C types, so both sides can include it.
 */
#ifndef PORT_VOICES_H
#define PORT_VOICES_H

/* The DSP mixes 24 channels, in frames of 160 samples at its own rate (SYSCLOCK_SOC / 512 Hz, about 32728). */
#define PV_CHANNELS 24
#define PV_FRAME_SAMPLES 160
#define PV_OUTPUT_RATE 32728.498f

/* The 12 mixing volumes of a channel, as ndsp has them. */
enum {
    PV_MIX_FRONT_LEFT = 0,
    PV_MIX_FRONT_RIGHT = 1,
    PV_MIX_AUX0 = 4,  /* front left, front right, back left, back right of aux bus 0 */
    PV_MIX_AUX1 = 8,  /* the same for aux bus 1 */
    PV_MIX_COUNT = 12
};

/******************************** Game side (src/audio/voices.c) ********************************/

/* Whether the player asked for the DSP voices (settings.txt sound_dsp, DKR_AUDIO_DSP). Fixed at start-up. */
int pv_enabled(void);
/* Whether the engine runs: pv_enabled() and the backend started. */
int pv_active(void);
/* Called by the backend once per frame of PV_FRAME_SAMPLES output samples, from the thread that owns the channels. */
void pv_frame_tick(void);
/* The N64 reverb of an aux bus, on the bus's front left and right returns in place (unclipped 32 bit samples). */
void pv_aux_process(int bus, int *left, int *right, int count);
/* The game's audio output in this mode: osAiSetNextBuffer sizes and what is still to be played, both in bytes. */
void pv_output_queue(unsigned numBytes);
unsigned pv_output_remaining(void);
/* After the DSP was unloaded and reloaded (HOME menu, sleep): restarts the playing voices from a known point. */
void pv_resync(void);
/* For the results label: e.g. "dsp" with the peak channel count. */
const char *pv_describe(void);

/******************************** Backend ********************************/

/* Starts the backend for a synthesizer running at synthRate. Returns 0 if it cannot (no DSP). */
int pv_backend_init(unsigned synthRate);
/* Memory for decoded samples that the channels can read; data is flushed with pv_backend_commit after writing. */
void *pv_backend_alloc(unsigned bytes);
void pv_backend_free(void *data);
void pv_backend_commit(const void *data, unsigned bytes);
/*
 * Plays a sample on a channel from its start, at rate Hz with the given mix: head (headSamples of mono 16 bit PCM)
 * once, then loop (loopSamples) over and over if there is one. Either part may be missing (0 samples).
 */
void pv_backend_play(int channel, const short *head, unsigned headSamples, const short *loop, unsigned loopSamples,
                     float rate, const float mix[PV_MIX_COUNT]);
void pv_backend_stop(int channel);
void pv_backend_set_rate(int channel, float rate);
void pv_backend_set_mix(int channel, const float mix[PV_MIX_COUNT]);
/* Frames of PV_FRAME_SAMPLES mixed so far. */
unsigned pv_backend_frames(void);

#endif
