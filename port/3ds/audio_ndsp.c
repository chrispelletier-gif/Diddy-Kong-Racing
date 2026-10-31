/*
 * Audio output through the DSP (ndsp). Each buffer handed to the emulated AI becomes one ndsp wave buffer.
 * If the DSP is unavailable (no dspfirm.cdc), output is discarded but timed as if it were played, so the
 * game's audio pacing still works.
 *
 * Also the Settings menu's Sound page (roadmap P9 and O5): which core mixes, the mixer rate, the DSP's
 * resampling filter and a larger output buffer.
 */
#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat/options.h"
#include "plat/plat.h"
#include "plat/settings.h"
#include "audio/voices.h"

u32 port_audio_output_remaining_bytes(void);
static u32 queued_bytes(void);
int plat_3ds_audio_on_system_core(void);
void port_voices_start(u32 synthRate);
static void voices_apply_interp(void);

#define NUM_WAVEBUFS 8
#define WAVEBUF_BYTES 8192

static ndspWaveBuf sWaveBufs[NUM_WAVEBUFS];
static u8 *sSampleData;
static u32 sWaveBufBytes[NUM_WAVEBUFS];
static int sNextBuf;
static volatile int sDspAvailable;
static u32 sFrequency = 22050;
static FILE *sVoiceDump; /* DKR_DUMP_AUDIO with the DSP voices */

/* Live settings: the DSP's polyphase filter instead of linear interpolation, and extra queued sound. */
static volatile int sSmooth = -1;
static volatile int sLargeBuffer = -1;

static void load_settings(void) {
    if (sSmooth < 0) {
        sSmooth = settings_get("sound_smooth", 1) != 0;
        sLargeBuffer = settings_get("sound_buffer", 0) != 0;
    }
}

/* The mixer rate setting: 0 the game's own (22050 Hz), 1 32000 Hz. DKR_AUDIO_RATE overrides it. With the DSP voices
 * the game's synthesizer keeps its own rate (the DSP resamples every voice to its output rate anyway). */
u32 port_audio_output_rate(u32 requested) {
    const char *env = plat_getenv("DKR_AUDIO_RATE");
    if (pv_enabled()) {
        return requested;
    }
    if (env != NULL && atoi(env) > 0) {
        return (u32) atoi(env);
    }
    return settings_get("sound_rate", 0) ? 32000 : requested;
}

static void apply_interp(void) {
    ndspChnSetInterp(0, sSmooth ? NDSP_INTERP_POLYPHASE : NDSP_INTERP_LINEAR);
    voices_apply_interp();
}

/* Fallback clock used when the DSP is unavailable. */
static unsigned long long sFakeStart;
static unsigned long long sFakeQueued;

/* Tells the player on the bottom screen why there is no sound, spotting a misnamed firmware file if there is one. */
static void explain_missing_dsp_firmware(void) {
    DIR *dir;
    struct dirent *entry;
    char misnamed[256] = "";

    dir = opendir("sdmc:/3ds");
    if (dir != NULL) {
        while ((entry = readdir(dir)) != NULL) {
            if (strncasecmp(entry->d_name, "dspfirm", 7) == 0 && strcmp(entry->d_name, "dspfirm.cdc") != 0) {
                snprintf(misnamed, sizeof(misnamed), "%s", entry->d_name);
                break;
            }
        }
        closedir(dir);
    }
    plat_status("No sound: DSP firmware not found.");
    if (misnamed[0] != '\0') {
        plat_status("Found sdmc:/3ds/%s: rename it to exactly dspfirm.cdc (Windows may be hiding an extension)",
                    misnamed);
    } else {
        plat_status("Put dspfirm.cdc in sdmc:/3ds/");
    }
}

void port_audio_output_init(u32 frequency) {
    float mix[12];
    int i;

    sFrequency = frequency;
    load_settings();
    if (ndspInit() != 0) {
        plat_log("ndspInit failed, audio disabled");
        explain_missing_dsp_firmware();
        return;
    }
    sSampleData = linearAlloc(NUM_WAVEBUFS * WAVEBUF_BYTES);
    if (sSampleData == NULL) {
        ndspExit();
        return;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    apply_interp();
    ndspChnSetRate(0, (float) frequency);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(0, mix);
    memset(sWaveBufs, 0, sizeof(sWaveBufs));
    for (i = 0; i < NUM_WAVEBUFS; i++) {
        sWaveBufs[i].data_vaddr = sSampleData + i * WAVEBUF_BYTES;
        sWaveBufs[i].status = NDSP_WBUF_DONE;
    }
    sDspAvailable = 1;
    port_voices_start(frequency);
    plat_status(pv_active() ? "Sound started (DSP voices)" : "Sound started");
}

/* The application is closing. Later calls see no DSP and only count time. */
static aptHookCookie sVoiceAptCookie;

void port_audio_output_exit(void) {
    if (sDspAvailable) {
        int i;
        sDspAvailable = 0;
        ndspSetCallback(NULL, NULL);
        aptUnhook(&sVoiceAptCookie);
        for (i = 0; i < PV_CHANNELS; i++) {
            ndspChnWaveBufClear(i);
        }
        ndspExit();
        if (sVoiceDump != NULL) {
            fclose(sVoiceDump);
            sVoiceDump = NULL;
        }
    }
}

/*
 * From the crash handler (bugreport_3ds.c): the DSP would go on playing the channels' looping samples after the
 * program ends, so its program is unloaded. Only a service call, no locks: the handler cannot take any.
 */
void plat_audio_crash_silence(void) {
    if (sDspAvailable) {
        DSP_UnloadComponent();
    }
}

static unsigned long long fake_played_bytes(void) {
    if (sFakeStart == 0) {
        return 0;
    }
    return (plat_time_ns() - sFakeStart) * sFrequency / 1000000000ULL * 4;
}

void port_audio_output_queue(const s16 *samples, u32 numBytes) {
    ndspWaveBuf *wb;

    if (pv_active()) {
        // The DSP voices play the sound; this buffer only keeps the game's audio clock.
        pv_output_queue(numBytes);
        return;
    }
    if (!sDspAvailable) {
        if (sFakeStart == 0 || fake_played_bytes() > sFakeQueued) {
            sFakeStart = plat_time_ns();
            sFakeQueued = 0;
        }
        sFakeQueued += numBytes;
        return;
    }

    wb = &sWaveBufs[sNextBuf];
    if (wb->status == NDSP_WBUF_QUEUED || wb->status == NDSP_WBUF_PLAYING) {
        // Every buffer is still in flight: the game is producing faster than the DSP plays. Drop this one.
        gPlatCounters[PLAT_COUNTER_AUDIO_DROPPED]++;
        return;
    }
    if (ndspChnIsPlaying(0) == false || queued_bytes() == 0) {
        gPlatCounters[PLAT_COUNTER_AUDIO_UNDERRUN]++;
    }
    if (numBytes > WAVEBUF_BYTES) {
        numBytes = WAVEBUF_BYTES;
    }
    memcpy((void *) wb->data_vaddr, samples, numBytes);
    DSP_FlushDataCache(wb->data_vaddr, numBytes);
    wb->nsamples = numBytes / 4;
    sWaveBufBytes[sNextBuf] = numBytes;
    ndspChnWaveBufAdd(0, wb);
    sNextBuf = (sNextBuf + 1) % NUM_WAVEBUFS;
}

static u32 queued_bytes(void) {
    u32 total = 0;
    int i;

    if (!sDspAvailable) {
        unsigned long long played = fake_played_bytes();
        return played >= sFakeQueued ? 0 : (u32) (sFakeQueued - played);
    }
    for (i = 0; i < NUM_WAVEBUFS; i++) {
        u8 status = sWaveBufs[i].status;
        if (status == NDSP_WBUF_QUEUED || status == NDSP_WBUF_PLAYING) {
            total += sWaveBufBytes[i];
            if (status == NDSP_WBUF_PLAYING) {
                u32 pos = ndspChnGetSamplePos(0) * 4;
                total -= (pos < sWaveBufBytes[i]) ? pos : sWaveBufBytes[i];
            }
        }
    }
    return total;
}

/*
 * What the game sees as still to play. With the large buffer, 50 ms less than is queued: the game sizes each audio
 * frame to top the output up to its usual level, so it settles with that much more queued, which covers a late
 * audio frame at the cost of the sound coming 50 ms later.
 */
u32 port_audio_output_remaining_bytes(void) {
    u32 queued;
    if (pv_active()) {
        return pv_output_remaining();
    }
    queued = queued_bytes();
    u32 cushion = sLargeBuffer > 0 ? (sFrequency / 20) * 4 : 0;
    return queued > cushion ? queued - cushion : 0;
}

/******************************** The Sound page ********************************/

enum { SOUND_ENGINE, SOUND_CORE, SOUND_RATE, SOUND_SMOOTH, SOUND_BUFFER, SOUND_ROWS };

/* A player's build (plat.h PLAT_DEBUG_TOOLS) shows only smooth resampling; the rest are performance switches, kept at
 * their defaults (settings.c). The page's functions number the rows shown; sound_row() gives the row of all of them. */
static int sound_hidden(int row) {
    return !PLAT_DEBUG_TOOLS && row != SOUND_SMOOTH;
}

static int sound_row(int i) {
    return options_visible_row(SOUND_ROWS, sound_hidden, i);
}

int sound_option_count(void) {
    return options_visible_count(SOUND_ROWS, sound_hidden);
}

const char *sound_option_label(int i) {
    static const char *const sLabels[SOUND_ROWS] = { "Sound engine (restart)", "Mixing core (restart)",
                                                     "Mixer rate (restart)", "Smooth resampling", "Sound buffer" };
    i = sound_row(i);
    return (i >= 0 && i < SOUND_ROWS) ? sLabels[i] : "";
}

/* The rows the pause menu shows (plat/options.h): the others only change after a restart. */
int sound_option_in_race(int i) {
    i = sound_row(i);
    return i == SOUND_SMOOTH || i == SOUND_BUFFER;
}

/* The mixing core, rate and buffer rows are the CPU mixer's: with the DSP voices they say so and do nothing. */
static int cpu_mixer_only(int i) {
    // On an Old 3DS the mixing core moves the DSP voices' own CPU work too (the audio manager and the event thread,
    // see plat_init), so it is a choice there either way.
    if (i == SOUND_CORE && plat_old_3ds()) {
        return 0;
    }
    return (i == SOUND_CORE || i == SOUND_RATE || i == SOUND_BUFFER) && settings_get("sound_dsp", 1);
}

void sound_option_value(int i, char *out, int size) {
    i = sound_row(i);
    load_settings();
    if (cpu_mixer_only(i)) {
        snprintf(out, size, "CPU mixer only");
        return;
    }
    switch (i) {
        case SOUND_ENGINE:
            snprintf(out, size, "%s", settings_get("sound_dsp", 1) ? "DSP voices" : "CPU mixer");
            break;
        case SOUND_CORE:
            snprintf(out, size, "%s", settings_get("sound_core", 0) ? "System" : "Renderer");
            break;
        case SOUND_RATE:
            snprintf(out, size, "%s", settings_get("sound_rate", 0) ? "32 kHz" : "22 kHz");
            break;
        case SOUND_SMOOTH:
            snprintf(out, size, "%s", sSmooth ? "ON" : "OFF");
            break;
        case SOUND_BUFFER:
            snprintf(out, size, "%s", sLargeBuffer ? "Large" : "Normal");
            break;
        default:
            out[0] = '\0';
            break;
    }
}

void sound_option_change(int i, int step) {
    (void) step; // Every row has two values.
    i = sound_row(i);
    load_settings();
    if (cpu_mixer_only(i)) {
        return;
    }
    switch (i) {
        case SOUND_ENGINE:
            settings_set("sound_dsp", !settings_get("sound_dsp", 1));
            break;
        case SOUND_CORE:
            settings_set("sound_core", !settings_get("sound_core", 0));
            break;
        case SOUND_RATE:
            settings_set("sound_rate", !settings_get("sound_rate", 0));
            break;
        case SOUND_SMOOTH:
            sSmooth = !sSmooth;
            settings_set("sound_smooth", sSmooth);
            if (sDspAvailable) {
                apply_interp();
            }
            break;
        case SOUND_BUFFER:
            sLargeBuffer = !sLargeBuffer;
            settings_set("sound_buffer", sLargeBuffer);
            break;
    }
}

/* For the test runs' results: core, rate, filter and buffer the sound is running with, e.g. "sys/22k/smooth". */
const char *plat_audio_setup(void) {
    static char sText[48];
    int systemCore = plat_3ds_audio_on_system_core();
    load_settings();
    if (pv_active()) {
        snprintf(sText, sizeof(sText), "%s/%s%s", pv_describe(), sSmooth ? "smooth" : "linear", systemCore ? "/sys" : "");
        return sText;
    }
    snprintf(sText, sizeof(sText), "%s/%uk/%s%s", systemCore ? "sys" : "rdp", (unsigned) (sFrequency / 1000),
             sSmooth ? "smooth" : "linear", sLargeBuffer > 0 ? "/large" : "");
    return sText;
}

/******************************** The DSP voices (voices.h) ********************************/

/*
 * libctru's ndsp internals (ndsp-internal.h): the DSP's shared memory regions, double buffered by frame. Region 7
 * holds the aux buses' samples: each frame the DSP writes the buses' sends into the region it owns (ndspBufferId),
 * and reads the application's returns (here: the N64 reverb of those sends) from the one the application fills
 * (ndspBufferCurId). libctru itself leaves these buses untouched ("TODO: call aux user callback").
 */
extern void *ndspVars[16][2];
extern u16 ndspFrameId, ndspBufferCurId, ndspBufferId;
#define NDSP_AUX_REGION 7
#define NDSP_OUTPUT_REGION 6
#define AUX_BUS_WORDS (4 * PV_FRAME_SAMPLES) /* front left, front right, back left, back right */

static ndspWaveBuf sVoiceBufs[PV_CHANNELS][2];
static volatile u32 sVoiceFrames;

static void voices_apply_interp(void) {
    int i;
    if (pv_active()) {
        for (i = 0; i < PV_CHANNELS; i++) {
            ndspChnSetInterp(i, sSmooth ? NDSP_INTERP_POLYPHASE : NDSP_INTERP_LINEAR);
        }
    }
}

/* Every DSP frame, on ndsp's thread: the voices' parameter changes, then the reverbs on the aux returns. */
/*
 * The HOME menu and sleep unload the DSP (libctru's APT handling); on the way back it is reloaded and libctru
 * resends its channel settings, but which samples go on playing is not reliable: voices cut out, and some played on
 * with libctru's bookkeeping of them lost. So the engine restarts its voices once the DSP runs again. The hook runs on
 * the APT thread before the DSP is back, so it only leaves a note for the frame callback.
 */
static volatile int sVoiceResumePending;

static void voices_apt_hook(APT_HookType hook, void *param) {
    (void) param;
    if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
        sVoiceResumePending = 1;
    }
}

static void voices_frame(void *arg) {
    static u16 sLastFrameId = 0xFFFF;
    int bus;
    (void) arg;

    // ndsp calls this from its loop even when it is not running a frame (ndspFrameId unchanged): skip those.
    if (ndspFrameId == sLastFrameId) {
        return;
    }
    sLastFrameId = ndspFrameId;
    if (sVoiceResumePending) {
        // Every channel is set up again from scratch (ndspChnReset sends the DSP its init-params flag too) rather
        // than merged with whatever the firmware restored, and the aux buses re-enabled (libctru does not resend
        // effect settings on a resume); then the engine restarts the voices that were playing.
        int i;
        sVoiceResumePending = 0;
        for (i = 0; i < PV_CHANNELS; i++) {
            ndspChnReset(i);
            ndspChnSetFormat(i, NDSP_FORMAT_MONO_PCM16);
            ndspChnSetInterp(i, sSmooth ? NDSP_INTERP_POLYPHASE : NDSP_INTERP_LINEAR);
        }
        for (i = 0; i < 2; i++) {
            ndspAuxSetEnable(i, true);
            ndspAuxSetFrontBypass(i, false);
            ndspAuxSetVolume(i, 1.0f);
        }
        pv_resync();
    }
    pv_frame_tick();
    if (ndspVars[NDSP_AUX_REGION][0] == NULL || ndspVars[NDSP_AUX_REGION][1] == NULL) {
        sVoiceFrames++;
        return;
    }
    for (bus = 0; bus < 2; bus++) {
        int *in = (int *) ndspVars[NDSP_AUX_REGION][ndspBufferId] + bus * AUX_BUS_WORDS;
        int *out = (int *) ndspVars[NDSP_AUX_REGION][ndspBufferCurId] + bus * AUX_BUS_WORDS;
        pv_aux_process(bus, in, in + PV_FRAME_SAMPLES, PV_FRAME_SAMPLES);
        if (out != in) {
            memcpy(out, in, 2 * PV_FRAME_SAMPLES * sizeof(int));
        }
        memset(out + 2 * PV_FRAME_SAMPLES, 0, 2 * PV_FRAME_SAMPLES * sizeof(int));
    }
    if (sVoiceDump != NULL && ndspVars[NDSP_OUTPUT_REGION][ndspBufferId] != NULL) {
        // DKR_DUMP_AUDIO: the DSP's finished stereo output of the last frame.
        fwrite(ndspVars[NDSP_OUTPUT_REGION][ndspBufferId], 1, PV_FRAME_SAMPLES * 4, sVoiceDump);
    }
    sVoiceFrames++;
}

int pv_backend_init(unsigned synthRate) {
    const char *dump = plat_getenv("DKR_DUMP_AUDIO");
    int i;
    (void) synthRate;

    if (!sDspAvailable) {
        return 0;
    }
    for (i = 0; i < PV_CHANNELS; i++) {
        ndspChnReset(i);
        ndspChnSetFormat(i, NDSP_FORMAT_MONO_PCM16);
        ndspChnSetInterp(i, sSmooth ? NDSP_INTERP_POLYPHASE : NDSP_INTERP_LINEAR);
    }
    for (i = 0; i < 2; i++) {
        ndspAuxSetEnable(i, true);
        ndspAuxSetFrontBypass(i, false);
        ndspAuxSetVolume(i, 1.0f);
    }
    if (dump != NULL && dump[0] != '\0') {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", plat_data_dir(), dump);
        sVoiceDump = fopen(path, "wb");
        if (sVoiceDump != NULL) {
            setvbuf(sVoiceDump, NULL, _IOFBF, 64 * 1024);
            plat_log("audio: dumping the DSP output to %s", path);
        }
    }
    ndspSetCallback(voices_frame, NULL);
    aptHook(&sVoiceAptCookie, voices_apt_hook, NULL);
    plat_log("audio: DSP voices on %d channels, %lu KB of linear memory left for textures", PV_CHANNELS,
             (unsigned long) (linearSpaceFree() / 1024));
    return 1;
}

void *pv_backend_alloc(unsigned bytes) {
    return linearAlloc((bytes + 31) & ~31u);
}

void pv_backend_free(void *data) {
    if (data != NULL) {
        linearFree(data);
    }
}

void pv_backend_commit(const void *data, unsigned bytes) {
    DSP_FlushDataCache(data, bytes);
}

void pv_backend_play(int channel, const s16 *head, unsigned headSamples, const s16 *loop, unsigned loopSamples,
                     float rate, const float mix[PV_MIX_COUNT]) {
    ndspWaveBuf *first = &sVoiceBufs[channel][0];
    ndspWaveBuf *second = &sVoiceBufs[channel][1];

    ndspChnWaveBufClear(channel);
    ndspChnSetRate(channel, rate);
    ndspChnSetMix(channel, (float *) mix);
    if (headSamples != 0) {
        memset(first, 0, sizeof(*first));
        first->data_vaddr = head;
        first->nsamples = headSamples;
        first->status = NDSP_WBUF_DONE;
        ndspChnWaveBufAdd(channel, first);
    }
    if (loopSamples != 0) {
        memset(second, 0, sizeof(*second));
        second->data_vaddr = loop;
        second->nsamples = loopSamples;
        second->looping = true;
        second->status = NDSP_WBUF_DONE;
        ndspChnWaveBufAdd(channel, second);
    }
}

void pv_backend_stop(int channel) {
    ndspChnWaveBufClear(channel);
}

void pv_backend_set_rate(int channel, float rate) {
    ndspChnSetRate(channel, rate);
}

void pv_backend_set_mix(int channel, const float mix[PV_MIX_COUNT]) {
    ndspChnSetMix(channel, (float *) mix);
}

unsigned pv_backend_frames(void) {
    return sVoiceFrames;
}
