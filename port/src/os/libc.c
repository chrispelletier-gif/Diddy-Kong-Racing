/* C replacements for the libultra libc routines that are written in MIPS assembly. */
#include <ultra64.h>

void bcopy(const void *src, void *dst, int len) {
    __builtin_memmove(dst, src, len);
}

void bzero(void *dst, int len) {
    __builtin_memset(dst, 0, len);
}

int bcmp(const void *a, const void *b, int len) {
    return __builtin_memcmp(a, b, len);
}

/* The libm float NaN constant used by libultra's sinf/cosf. */
const float __libm_qnan_f = __builtin_nanf("");
