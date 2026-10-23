#ifndef _MACROS_H_
#define _MACROS_H_

#include <PRinternal/macros.h>

#ifndef __sgi
#define GLOBAL_ASM(...)
#endif

#if !defined(__sgi) && (!defined(NON_MATCHING) || !defined(AVOID_UB))
// asm-process isn't supported outside of IDO, and undefined behavior causes
// crashes.
// #error Matching build is only possible on IDO; please build with NON_MATCHING=1.
#endif

#define ARRAY_COUNT(arr) (s32)(sizeof(arr) / sizeof(arr[0]))

#define GLUE(a, b) a ## b
#define GLUE2(a, b) GLUE(a, b)

// Avoid undefined behaviour for non-returning functions
#ifdef __GNUC__
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN
#endif

// Static assertions
#ifdef __GNUC__
#define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#define STATIC_ASSERT(cond, msg) typedef char GLUE2(static_assertion_failed, __LINE__)[(cond) ? 1 : -1]
#endif

// Align to 8-byte boundary for DMA requirements
#ifdef __GNUC__
#define ALIGNED8 __attribute__((aligned(8)))
#else
#define ALIGNED8
#endif

// Align to 16-byte boundary for audio lib requirements
#ifdef __GNUC__
#define ALIGNED16 __attribute__((aligned(16)))
#else
#define ALIGNED16
#endif

// Use built-in pseudocode where possible in NON_MATCHING builds
#if defined(__GNUC__) && !defined(CC_CHECK)
#define abs(x)      __builtin_abs(x)
#define fabsf(x)    __builtin_fabsf(x)
#define sqrtf(f)    __builtin_sqrtf(f)
#undef ABS
#undef ABSF
#define ABS(x)      __builtin_abs(x)
#define ABSF(x)     __builtin_fabsf(x)
#else
#define inline
#define ABSF(x) (x < 0.f ? -x : x)
#define ABS(x) ((x) >= 0 ? (x) : -(x))
#endif


#ifdef TARGET_PORT
// Native ports have no KSEG0/physical split.
#define VIRTUAL_TO_PHYSICAL(addr)   ((uintptr_t)(addr))
#define PHYSICAL_TO_VIRTUAL(addr)   ((uintptr_t)(addr))
#define VIRTUAL_TO_PHYSICAL2(addr)  ((u8 *)(addr))
#else
// convert a virtual address to physical.
#define VIRTUAL_TO_PHYSICAL(addr)   ((uintptr_t)(addr) & 0x1FFFFFFF)

// convert a physical address to virtual.
#define PHYSICAL_TO_VIRTUAL(addr)   ((uintptr_t)(addr) | 0x80000000)

// another way of converting virtual to physical
#define VIRTUAL_TO_PHYSICAL2(addr)  ((u8 *)(addr) - 0x80000000U)
#endif

// Used to suppress warnings in the ./generate_ctx.sh script.
#define INCONSISTENT 

// Used to make a u32 colour value look clearer. Transforms 0xFF0000FF to 255, 0, 0, 255
#define COLOUR_RGBA32(r, g, b, a) ((u32)((r << 24) | (g << 16) |  (b << 8) | a))

// ColourRGBA.word only equals COLOUR_RGBA32(r, g, b, a) on big endian machines. These convert explicitly.
#ifdef TARGET_PORT
#define COLOUR_WORD(c) COLOUR_RGBA32((u32) (c).r, (u32) (c).g, (u32) (c).b, (u32) (c).a)
#define COLOUR_SET_WORD(c, w) ((c).r = (u8) ((w) >> 24), (c).g = (u8) ((w) >> 16), (c).b = (u8) ((w) >> 8), (c).a = (u8) (w))
#else
#define COLOUR_WORD(c) ((c).word)
#define COLOUR_SET_WORD(c, w) ((c).word = (w))
#endif

// Float to s16 for values that can be out of range, where the N64 kept the low 16 bits. A plain conversion of such a
// value is undefined behaviour, which optimised port builds can turn into wild memory reads.
#ifdef TARGET_PORT
#define F2S16(x) ((s16) port_f2i32(x))
#else
#define F2S16(x) ((s16) (x))
#endif

// A few systems in the game use an array as a cache table. This gives you the asset ID
#define ASSETCACHE_ID(x)    ((x << 1) + 0)
// A few systems in the game use an array as a cache table. This gives you the pointer to the asset
#define ASSETCACHE_PTR(x)   ((x << 1) + 1)

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#endif
