/*
 * Audio output through the DSP (ndsp). Each buffer handed to the emulated AI becomes one ndsp wave buffer.
 * If the DSP is unavailable (no dspfirm.cdc), output is discarded but timed as if it were played, so the
 * game's audio pacing still works.
 */
#include <3ds.h>
#include <string.h>

#include "plat/plat.h"

#define NUM_WAVEBUFS 8
#define WAVEBUF_BYTES 8192

static ndspWaveBuf sWaveBufs[NUM_WAVEBUFS];
static u8 *sSampleData;
static u32 sWaveBufBytes[NUM_WAVEBUFS];
static int sNextBuf;
static int sDspAvailable;
static u32 sFrequency = 22050;

/* Fallback clock used when the DSP is unavailable. */
static unsigned long long sFakeStart;
static unsigned long long sFakeQueued;

void port_audio_output_init(u32 frequency) {
    float mix[12];
    int i;

    sFrequency = frequency;
    if (ndspInit() != 0) {
        plat_log("ndspInit failed, audio disabled");
        return;
    }
    sSampleData = linearAlloc(NUM_WAVEBUFS * WAVEBUF_BYTES);
    if (sSampleData == NULL) {
        ndspExit();
        return;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
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
        return;
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

u32 port_audio_output_remaining_bytes(void) {
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
