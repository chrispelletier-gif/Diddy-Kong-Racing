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

#include "plat/plat.h"
#include "plat/settings.h"

u32 port_audio_output_remaining_bytes(void);
static u32 queued_bytes(void);
int plat_3ds_audio_on_system_core(void);

#define NUM_WAVEBUFS 8
#define WAVEBUF_BYTES 8192

static ndspWaveBuf sWaveBufs[NUM_WAVEBUFS];
static u8 *sSampleData;
static u32 sWaveBufBytes[NUM_WAVEBUFS];
static int sNextBuf;
static volatile int sDspAvailable;
static u32 sFrequency = 22050;

/* Live settings: the DSP's polyphase filter instead of linear interpolation, and extra queued sound. */
static volatile int sSmooth = -1;
static volatile int sLargeBuffer = -1;

static void load_settings(void) {
    if (sSmooth < 0) {
        sSmooth = settings_get("sound_smooth", 1) != 0;
        sLargeBuffer = settings_get("sound_buffer", 0) != 0;
    }
}

/* The mixer rate setting: 0 the game's own (22050 Hz), 1 32000 Hz. DKR_AUDIO_RATE overrides it. */
u32 port_audio_output_rate(u32 requested) {
    const char *env = plat_getenv("DKR_AUDIO_RATE");
    if (env != NULL && atoi(env) > 0) {
        return (u32) atoi(env);
    }
    return settings_get("sound_rate", 0) ? 32000 : requested;
}

static void apply_interp(void) {
    ndspChnSetInterp(0, sSmooth ? NDSP_INTERP_POLYPHASE : NDSP_INTERP_LINEAR);
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
    plat_status("Sound started");
}

/* The application is closing. Later calls see no DSP and only count time. */
void port_audio_output_exit(void) {
    if (sDspAvailable) {
        sDspAvailable = 0;
        ndspChnWaveBufClear(0);
        ndspExit();
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
    u32 queued = queued_bytes();
    u32 cushion = sLargeBuffer > 0 ? (sFrequency / 20) * 4 : 0;
    return queued > cushion ? queued - cushion : 0;
}

/******************************** The Sound page ********************************/

enum { SOUND_CORE, SOUND_RATE, SOUND_SMOOTH, SOUND_BUFFER, SOUND_ROWS };

int sound_option_count(void) {
    return SOUND_ROWS;
}

const char *sound_option_label(int i) {
    static const char *const sLabels[SOUND_ROWS] = { "Mixing core (restart)", "Mixer rate (restart)",
                                                     "Smooth resampling", "Sound buffer" };
    return (i >= 0 && i < SOUND_ROWS) ? sLabels[i] : "";
}

void sound_option_value(int i, char *out, int size) {
    load_settings();
    switch (i) {
        case SOUND_CORE:
            snprintf(out, size, "%s", settings_get("sound_core", 1) ? "System" : "Renderer");
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
    load_settings();
    switch (i) {
        case SOUND_CORE:
            settings_set("sound_core", !settings_get("sound_core", 1));
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
    snprintf(sText, sizeof(sText), "%s/%uk/%s%s", systemCore ? "sys" : "rdp", (unsigned) (sFrequency / 1000),
             sSmooth ? "smooth" : "linear", sLargeBuffer > 0 ? "/large" : "");
    return sText;
}
