#ifndef PORT_AUDIO_SIMD_H
#define PORT_AUDIO_SIMD_H

/*
 * The few ARMv6 SIMD instructions the audio microcode uses (roadmap P10), as inline functions. On an ARM with the
 * SIMD extension they are the instructions themselves; elsewhere C that computes exactly the same bits, so the host
 * build runs the same mixing code. A packed pair holds the first sample in its low half, as a 32 bit load of two
 * consecutive samples does on a little endian CPU.
 */
#include <string.h>
#include "types.h"

#if defined(__ARM_FEATURE_SIMD32) && defined(__ARM_FEATURE_DSP) && !defined(__thumb__)
#define SIMD_NATIVE 1
#else
#define SIMD_NATIVE 0
#endif

static inline u32 simd_pack(s32 lo, s32 hi) {
    return ((u32) lo & 0xFFFF) | ((u32) hi << 16);
}

/* Two consecutive samples (the address needs only 2 byte alignment; ARMv6 loads unaligned words). */
static inline u32 simd_load_pair(const s16 *p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}

static inline void simd_store_pair(s16 *p, u32 v) {
    memcpy(p, &v, 4);
}

#if SIMD_NATIVE

/* a.lo * b.lo + a.hi * b.hi (SMUAD), and the same added to acc (SMLAD). The sums wrap. */
static inline s32 simd_smuad(u32 a, u32 b) {
    s32 r;
    __asm__("smuad %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

static inline s32 simd_smlad(u32 a, u32 b, s32 acc) {
    s32 r;
    __asm__("smlad %0, %1, %2, %3" : "=r"(r) : "r"(a), "r"(b), "r"(acc));
    return r;
}

/* Each half of a plus the same half of b, saturated to 16 bits (QADD16). */
static inline u32 simd_qadd16(u32 a, u32 b) {
    u32 r;
    __asm__("qadd16 %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

/*
 * Each half of b times gain, >> 15, given gain2 = gain * 2: SMULWB/SMULWT take the top 32 bits of a 32x16 bit
 * product, which is (gain * 2 * x) >> 16. Both halves of the result are exact unless gain and x are both -0x8000.
 */
static inline u32 simd_mul_q15(s32 gain2, u32 b) {
    s32 lo, hi;
    u32 r;
    __asm__("smulwb %0, %1, %2" : "=r"(lo) : "r"(gain2), "r"(b));
    __asm__("smulwt %0, %1, %2" : "=r"(hi) : "r"(gain2), "r"(b));
    __asm__("pkhbt %0, %1, %2, lsl #16" : "=r"(r) : "r"(lo), "r"(hi));
    return r;
}

#else

static inline s32 simd_lo(u32 v) {
    return (s16) (v & 0xFFFF);
}

static inline s32 simd_hi(u32 v) {
    return (s16) (v >> 16);
}

static inline s32 simd_smuad(u32 a, u32 b) {
    return (s32) ((u32) (simd_lo(a) * simd_lo(b)) + (u32) (simd_hi(a) * simd_hi(b)));
}

static inline s32 simd_smlad(u32 a, u32 b, s32 acc) {
    return (s32) ((u32) simd_smuad(a, b) + (u32) acc);
}

static inline s32 simd_sat16(s32 v) {
    return v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v;
}

static inline u32 simd_qadd16(u32 a, u32 b) {
    return simd_pack(simd_sat16(simd_lo(a) + simd_lo(b)), simd_sat16(simd_hi(a) + simd_hi(b)));
}

static inline u32 simd_mul_q15(s32 gain2, u32 b) {
    return simd_pack((s32) (((s64) gain2 * simd_lo(b)) >> 16), (s32) (((s64) gain2 * simd_hi(b)) >> 16));
}

#endif

#endif
