/*
 * Software implementation of the RSP audio microcode (aspMain, the libultra "ABI 1" command set).
 *
 * The behaviour follows the RSP code closely, including its fixed point rounding, so the mix sounds the same.
 * Everything here works in the host's byte order: the command list, the saved voice states and the ADPCM codebooks
 * are all native (see asset_swap.c), and only the compressed ADPCM bytes are big endian, which does not matter as
 * they are read one byte at a time.
 */
#pragma GCC optimize("O3")

#include <ultra64.h>
#include <PR/abi.h>
#include "types.h"
#include "port.h"

/* DMEM addresses are 16 bit; keep a little slack for the resampler reading 4 samples before its input. */
#define DMEM_SIZE 0x10000
static u8 sDmem[DMEM_SIZE + 64];

static struct {
    u16 in, out, count;
    u16 dryRight, wetLeft, wetRight;
    s16 vol[2], target[2];
    s32 rate[2];
    s16 dry, wet;
    s16 adpcmTable[16 * 8];
    u32 loop;
} sAudio;

/* The resampler's interpolation filter, taken from the microcode data in the ROM. */
static s16 sResampleLut[64 * 4];
static s32 sResampleLutLoaded;

static inline s16 clamp_s16(s32 v) {
#if defined(__arm__) && (__ARM_ARCH >= 6)
    // A single saturating instruction; this is the hottest operation in the mixer.
    s32 r;
    __asm__("ssat %0, #16, %1" : "=r"(r) : "r"(v));
    return (s16) r;
#endif
    if (v < -0x8000) {
        return -0x8000;
    }
    if (v > 0x7FFF) {
        return 0x7FFF;
    }
    return (s16) v;
}

static inline s16 *dmem_s16(u32 addr) {
    return (s16 *) (sDmem + (addr & 0xFFFE));
}

static inline u8 *dmem_u8(u32 addr) {
    return sDmem + (addr & 0xFFFF);
}

static inline void *ram(u32 addr) {
    return (void *) (uintptr_t) addr;
}

static void load_resample_lut(void) {
    // The table starts with these four taps in every version of the microcode.
    static const u8 sSignature[8] = { 0x0C, 0x39, 0x66, 0xAD, 0x0D, 0x46, 0xFF, 0xDF };
    const u8 *rom = port_rom_data();
    u32 size = port_rom_size();
    u32 i, j;

    sResampleLutLoaded = TRUE;
    for (i = 0; i + sizeof(sResampleLut) <= size; i += 2) {
        if (rom[i] == sSignature[0] && rom[i + 1] == sSignature[1]) {
            for (j = 2; j < sizeof(sSignature) && rom[i + j] == sSignature[j]; j++) {}
            if (j == sizeof(sSignature)) {
                for (j = 0; j < 64 * 4; j++) {
                    sResampleLut[j] = (s16) port_read_be16(rom + i + j * 2);
                }
                return;
            }
        }
    }
    port_log("audio: resampler table not found in the ROM, using linear interpolation");
    for (i = 0; i < 64; i++) {
        sResampleLut[i * 4 + 0] = 0;
        sResampleLut[i * 4 + 1] = (s16) (0x7FFF - i * 0x200);
        sResampleLut[i * 4 + 2] = (s16) (i * 0x200);
        sResampleLut[i * 4 + 3] = 0;
    }
}

static inline s32 rdot(s32 n, const s16 *x, const s16 *y) {
    s32 accu = 0;
    y += n;
    while (n != 0) {
        accu += *(x++) * *(--y);
        n--;
    }
    return accu;
}

/******************************** ADPCM ********************************/

static void adpcm_compute_residuals(s16 *dst, const s16 *src, const s16 *cbEntry, const s16 *lastSamples) {
    const s16 *book1 = cbEntry;
    const s16 *book2 = cbEntry + 8;
    s16 l1 = lastSamples[0];
    s16 l2 = lastSamples[1];
    s32 i;

    for (i = 0; i < 8; i++) {
        s32 accu = (s32) src[i] << 11;
        accu += book1[i] * l1 + book2[i] * l2 + rdot(i, book2, src);
        dst[i] = clamp_s16(accu >> 11);
    }
}

static void cmd_adpcm(u32 w0, u32 w1) {
    u8 flags = w0 >> 16;
    u32 lastFrameAddr = w1;
    u32 dmemo = sAudio.out;
    u32 dmemi = sAudio.in;
    s32 count = (sAudio.count + 31) & ~31;
    s16 lastFrame[16];
    s32 i;

    if (flags & A_INIT) {
        bzero(lastFrame, sizeof(lastFrame));
    } else {
        bcopy(ram((flags & A_LOOP) ? sAudio.loop : lastFrameAddr), lastFrame, sizeof(lastFrame));
    }
    for (i = 0; i < 16; i++, dmemo += 2) {
        *dmem_s16(dmemo) = lastFrame[i];
    }

    while (count > 0) {
        s16 frame[16];
        u8 code = *dmem_u8(dmemi++);
        u32 scale = (code & 0xF0) >> 4;
        u32 rshift = (scale < 12) ? 12 - scale : 0;
        const s16 *cbEntry = sAudio.adpcmTable + ((code & 0xF) << 4);

        for (i = 0; i < 8; i++) {
            u8 byte = *dmem_u8(dmemi++);
            frame[i * 2] = (s16) ((u16) (byte & 0xF0) << 8) >> rshift;
            frame[i * 2 + 1] = (s16) ((u16) (byte & 0x0F) << 12) >> rshift;
        }
        adpcm_compute_residuals(lastFrame, frame, cbEntry, lastFrame + 14);
        adpcm_compute_residuals(lastFrame + 8, frame + 8, cbEntry, lastFrame + 6);
        for (i = 0; i < 16; i++, dmemo += 2) {
            *dmem_s16(dmemo) = lastFrame[i];
        }
        count -= 32;
    }
    bcopy(lastFrame, ram(lastFrameAddr), sizeof(lastFrame));
}

/******************************** Resampler ********************************/

static void cmd_resample(u32 w0, u32 w1) {
    u8 flags = w0 >> 16;
    u32 pitch = (w0 & 0xFFFF) << 1;
    s16 *state = ram(w1);
    u16 ipos = (sAudio.in >> 1) - 4;
    u16 opos = sAudio.out >> 1;
    s32 count = ((sAudio.count + 15) & ~15) >> 1;
    u32 pitchAccu;
    s32 k;

    if (!sResampleLutLoaded) {
        load_resample_lut();
    }
    if (flags & A_INIT) {
        for (k = 0; k < 4; k++) {
            *dmem_s16((ipos + k) * 2) = 0;
        }
        pitchAccu = 0;
    } else {
        for (k = 0; k < 4; k++) {
            *dmem_s16((ipos + k) * 2) = state[k];
        }
        pitchAccu = (u16) state[4];
    }

    while (count != 0) {
        const s16 *lut = sResampleLut + ((pitchAccu & 0xFC00) >> 8);
        *dmem_s16(opos++ * 2) =
            clamp_s16(((*dmem_s16(ipos * 2) * lut[0]) + (*dmem_s16((ipos + 1) * 2) * lut[1]) +
                       (*dmem_s16((ipos + 2) * 2) * lut[2]) + (*dmem_s16((ipos + 3) * 2) * lut[3])) >>
                      15);
        pitchAccu += pitch;
        ipos += pitchAccu >> 16;
        pitchAccu &= 0xFFFF;
        count--;
    }

    for (k = 0; k < 4; k++) {
        state[k] = *dmem_s16((ipos + k) * 2);
    }
    state[4] = (s16) pitchAccu;
}

/******************************** Envelope mixer ********************************/

typedef struct Ramp {
    s32 value, step, target;
} Ramp;

static inline s16 ramp_step(Ramp *ramp) {
    s32 reached;
    ramp->value += ramp->step;
    reached = (ramp->step <= 0) ? (ramp->value <= ramp->target) : (ramp->value >= ramp->target);
    if (reached) {
        ramp->value = ramp->target;
        ramp->step = 0;
    }
    return (s16) (ramp->value >> 16);
}

static inline void sample_mix(s16 *dst, s16 src, s16 gain) {
    *dst = clamp_s16(*dst + ((src * gain) >> 15));
}

static void cmd_envmixer(u32 w0, u32 w1) {
    u8 flags = w0 >> 16;
    s32 aux = flags & A_AUX;
    s16 *save = ram(w1); // 40 halfwords of state, laid out as the microcode stores it.
    s16 *in = dmem_s16(sAudio.in);
    s16 *dl = dmem_s16(sAudio.out);
    s16 *dr = dmem_s16(sAudio.dryRight);
    s16 *wl = dmem_s16(sAudio.wetLeft);
    s16 *wr = dmem_s16(sAudio.wetRight);
    s16 dry, wet;
    Ramp ramps[2];
    s32 expSeq[2], expRates[2];
    s32 x, y, ptr = 0;

    if (flags & A_INIT) {
        dry = sAudio.dry;
        wet = sAudio.wet;
        ramps[0].value = sAudio.vol[0] << 16;
        ramps[1].value = sAudio.vol[1] << 16;
        ramps[0].target = sAudio.target[0] << 16;
        ramps[1].target = sAudio.target[1] << 16;
        expRates[0] = sAudio.rate[0];
        expRates[1] = sAudio.rate[1];
        expSeq[0] = sAudio.vol[0] * sAudio.rate[0];
        expSeq[1] = sAudio.vol[1] * sAudio.rate[1];
    } else {
        wet = save[0];
        dry = save[2];
        bcopy(save + 4, &ramps[0].target, 4);
        bcopy(save + 6, &ramps[1].target, 4);
        bcopy(save + 8, &expRates[0], 4);
        bcopy(save + 10, &expRates[1], 4);
        bcopy(save + 12, &expSeq[0], 4);
        bcopy(save + 14, &expSeq[1], 4);
        bcopy(save + 16, &ramps[0].value, 4);
        bcopy(save + 18, &ramps[1].value, 4);
    }
    ramps[0].step = ramps[0].target - ramps[0].value;
    ramps[1].step = ramps[1].target - ramps[1].value;

    for (y = 0; y < sAudio.count; y += 16) {
        if (ramps[0].step != 0) {
            expSeq[0] = (s32) (((s64) expSeq[0] * (s64) expRates[0]) >> 16);
            ramps[0].step = (expSeq[0] - ramps[0].value) >> 3;
        }
        if (ramps[1].step != 0) {
            expSeq[1] = (s32) (((s64) expSeq[1] * (s64) expRates[1]) >> 16);
            ramps[1].step = (expSeq[1] - ramps[1].value) >> 3;
        }
        for (x = 0; x < 8; x++, ptr++) {
            s16 lVol = ramp_step(&ramps[0]);
            s16 rVol = ramp_step(&ramps[1]);
            s16 src = in[ptr];
            sample_mix(&dl[ptr], src, clamp_s16((lVol * dry + 0x4000) >> 15));
            sample_mix(&dr[ptr], src, clamp_s16((rVol * dry + 0x4000) >> 15));
            if (aux) {
                sample_mix(&wl[ptr], src, clamp_s16((lVol * wet + 0x4000) >> 15));
                sample_mix(&wr[ptr], src, clamp_s16((rVol * wet + 0x4000) >> 15));
            }
        }
    }

    save[0] = wet;
    save[2] = dry;
    bcopy(&ramps[0].target, save + 4, 4);
    bcopy(&ramps[1].target, save + 6, 4);
    bcopy(&expRates[0], save + 8, 4);
    bcopy(&expRates[1], save + 10, 4);
    bcopy(&expSeq[0], save + 12, 4);
    bcopy(&expSeq[1], save + 14, 4);
    bcopy(&ramps[0].value, save + 16, 4);
    bcopy(&ramps[1].value, save + 18, 4);
}

/******************************** Pole filter ********************************/

static void cmd_polef(u32 w0, u32 w1) {
    u8 flags = w0 >> 16;
    u16 gain = w0;
    s16 *state = ram(w1);
    const s16 *h1 = sAudio.adpcmTable;
    s16 *h2 = sAudio.adpcmTable + 8;
    s16 h2Before[8];
    s16 *dst = dmem_s16(sAudio.out);
    u32 dmemi = sAudio.in;
    s32 count = (sAudio.count + 15) & ~15;
    s16 l1, l2;
    s32 i;

    if (sAudio.count == 0) {
        return;
    }
    if (flags & A_INIT) {
        l1 = 0;
        l2 = 0;
    } else {
        l1 = state[2];
        l2 = state[3];
    }
    for (i = 0; i < 8; i++) {
        h2Before[i] = h2[i];
        h2[i] = (s16) (((s32) h2[i] * gain) >> 14);
    }
    do {
        s16 frame[8];
        for (i = 0; i < 8; i++, dmemi += 2) {
            frame[i] = *dmem_s16(dmemi);
        }
        for (i = 0; i < 8; i++) {
            s32 accu = frame[i] * gain;
            accu += h1[i] * l1 + h2Before[i] * l2 + rdot(i, h2, frame);
            dst[i] = clamp_s16(accu >> 14);
        }
        l1 = dst[6];
        l2 = dst[7];
        dst += 8;
        count -= 16;
    } while (count > 0);
    bcopy(dst - 4, state, 8);
}

/******************************** Task entry ********************************/

void port_audio_run_task(void *taskPtr) {
    OSTask *task = (OSTask *) taskPtr;
    const u32 *cmd = (const u32 *) task->t.data_ptr;
    const u32 *end = (const u32 *) ((u8 *) task->t.data_ptr + task->t.data_size);
    u32 w0, w1, i, count;

    for (; cmd < end; cmd += 2) {
        w0 = cmd[0];
        w1 = cmd[1];
        switch (w0 >> 24) {
            case A_SPNOOP:
            case A_SEGMENT:
                // The port passes plain pointers, so segments are not needed.
                break;
            case A_ADPCM:
                cmd_adpcm(w0, w1);
                break;
            case A_CLEARBUFF:
                bzero(dmem_u8(w0 & ~3), ((w1 & 0xFFFF) + 3) & ~3);
                break;
            case A_ENVMIXER:
                cmd_envmixer(w0, w1);
                break;
            case A_LOADBUFF:
                count = (sAudio.count + 3) & ~3;
                bcopy(ram(w1 & ~3), dmem_u8(sAudio.in & ~3), count);
                if ((w0 >> 16) & PORT_A_SWAP16) {
                    port_swap16_array(dmem_u8(sAudio.in & ~3), count / 2);
                }
                break;
            case A_RESAMPLE:
                cmd_resample(w0, w1);
                break;
            case A_SAVEBUFF:
                bcopy(dmem_u8(sAudio.out & ~3), ram(w1 & ~3), (sAudio.count + 3) & ~3);
                break;
            case A_SETBUFF:
                if ((w0 >> 16) & A_AUX) {
                    sAudio.dryRight = w0;
                    sAudio.wetLeft = w1 >> 16;
                    sAudio.wetRight = w1;
                } else {
                    sAudio.in = w0;
                    sAudio.out = w1 >> 16;
                    sAudio.count = w1;
                }
                break;
            case A_SETVOL: {
                u8 flags = w0 >> 16;
                if (flags & A_AUX) {
                    sAudio.dry = (s16) w0;
                    sAudio.wet = (s16) w1;
                } else {
                    s32 lr = (flags & A_LEFT) ? 0 : 1;
                    if (flags & A_VOL) {
                        sAudio.vol[lr] = (s16) w0;
                    } else {
                        sAudio.target[lr] = (s16) w0;
                        sAudio.rate[lr] = (s32) w1;
                    }
                }
                break;
            }
            case A_DMEMMOVE:
                count = ((w1 & 0xFFFF) + 3) & ~3;
                bcopy(dmem_u8(w0 & 0xFFFF), dmem_u8(w1 >> 16), count);
                break;
            case A_LOADADPCM:
                count = w0 & 0xFFFF;
                if (count > sizeof(sAudio.adpcmTable)) {
                    count = sizeof(sAudio.adpcmTable);
                }
                bcopy(ram(w1), sAudio.adpcmTable, count);
                break;
            case A_MIXER: {
                s16 gain = (s16) w0;
                s16 *src = dmem_s16(w1 >> 16);
                s16 *dst = dmem_s16(w1 & 0xFFFF);
                count = sAudio.count >> 1;
                for (i = 0; i < count; i++) {
                    sample_mix(&dst[i], src[i], gain);
                }
                break;
            }
            case A_INTERLEAVE: {
                // The output may overlap an input, so interleave through a temporary buffer.
                static s16 sTemp[DMEM_SIZE / 2];
                s16 *left = dmem_s16(w1 >> 16);
                s16 *right = dmem_s16(w1 & 0xFFFF);
                count = sAudio.count >> 1;
                if (count > DMEM_SIZE / 4) {
                    count = DMEM_SIZE / 4;
                }
                for (i = 0; i < count; i++) {
                    sTemp[i * 2] = left[i];
                    sTemp[i * 2 + 1] = right[i];
                }
                bcopy(sTemp, dmem_u8(sAudio.out & ~3), count * 4);
                break;
            }
            case A_POLEF:
                cmd_polef(w0, w1);
                break;
            case A_SETLOOP:
                sAudio.loop = w1;
                break;
        }
    }
}
