#include "memory.h"

#ifdef __sgi
/**
 * Uses IDO specific -dollar compiler flag to get the current stack pointer.
 * Official Name: diCpuTraceCurrentStack
 */
StackInfo *stack_pointer(void) {
    return (StackInfo *) __$sp;
}
#else
/**
 * Uses GCC specific code to get the current stack pointer.
 * Official Name: diCpuTraceCurrentStack
 */
StackInfo *stack_pointer(void) {
#ifdef __mips__
    register StackInfo *sp;
    asm volatile("move %0, $sp\n" : "=r"(sp));
    return sp;
#else
    return (StackInfo *) __builtin_frame_address(0);
#endif
}
#endif
