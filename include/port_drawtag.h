#ifndef PORT_DRAWTAG_H
#define PORT_DRAWTAG_H
/*
 * Draw mix tags (port only): a no-op command in the display list that tells the renderer what the draws after it are
 * for (the track, a racer, another object, or anything else), so the profiler can count draws, triangles and vertices
 * per kind ("draw mix" log line). The renderer skips the command as the RDP would.
 */
#ifdef TARGET_PORT
#include "ultra64.h"
#define PORT_DRAW_TAG 0x50440000 /* "PD" */
enum { PORT_DRAW_OTHER, PORT_DRAW_TRACK, PORT_DRAW_RACERS, PORT_DRAW_OBJECTS };
#define port_draw_tag(dl, kind) gDPNoOpTag(dl, PORT_DRAW_TAG | (kind))
#else
#define port_draw_tag(dl, kind)
#endif
#endif
