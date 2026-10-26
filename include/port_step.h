#ifndef PORT_STEP_H
#define PORT_STEP_H
/*
 * 60 fps steps for the game's own code (see port.h). On the N64 every macro leaves its step as it is.
 */
#ifdef TARGET_PORT
#include "port.h"
#else
#define PORT_STEP(x) (x)
#define PORT_DECAY(k) (k)
#define PORT_LERP(k) (k)
#define PORT_TICK() (1)
#define PORT_SHIFT(x, n) ((x) >> (n))
#define PORT_HALF(x) (x)
#endif
#endif
