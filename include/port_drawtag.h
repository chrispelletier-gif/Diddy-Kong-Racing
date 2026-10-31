#ifndef PORT_DRAWTAG_H
#define PORT_DRAWTAG_H
/*
 * Draw mix tags (port only): a no-op command in the display list that tells the renderer what the draws after it are
 * for, so the profiler can show per kind the draws, triangles, vertices, the renderer's and the game thread's time and
 * the geometry that is wasted (off the screen, facing away, smaller than a pixel) ("draw mix" log line, the bottom
 * screen's monitor, the test runs' autorun_drawmix.csv). The renderer skips the command as the RDP would. Up to 16
 * kinds (the tag's low 4 bits); port_profile.c and gfx_rdp.c name them.
 */
#ifdef TARGET_PORT
#include "ultra64.h"
#define PORT_DRAW_TAG 0x50440000 /* "PD" */
enum {
    PORT_DRAW_OTHER,   /* Menus, text and anything untagged */
    PORT_DRAW_TRACK,   /* The level's opaque geometry */
    PORT_DRAW_RACERS,  /* Karts and their drivers */
    PORT_DRAW_OBJECTS, /* Other objects (bananas, balloons, scenery objects...) */
    PORT_DRAW_SKY,     /* The sky dome or background */
    PORT_DRAW_SEE_THROUGH, /* The level's see-through geometry and the water waves */
    PORT_DRAW_EFFECTS, /* Particles, weather, lens flare, shields and magnets */
    PORT_DRAW_SHADOWS, /* Shadows and water ripples under objects */
    PORT_DRAW_HUD,     /* The race HUD */
    PORT_DRAW_KINDS
};
/* The game thread's side: notes when each kind (and object kind, profile.h PROF_OBJ_*; -1 for none) starts, for the
 * time it spends building it (port_profile.c). */
void port_draw_kind_game(int kind, int object);
#define port_draw_tag(dl, kind)                                                                                        \
    do {                                                                                                               \
        gDPNoOpTag(dl, PORT_DRAW_TAG | (kind));                                                                        \
        port_draw_kind_game(kind, -1);                                                                                 \
    } while (0)
/* An object's draws: also its kind (its behaviour, or particles) in bits 4-11, plus one (0 is no object). */
#define port_draw_tag_object(dl, kind, object)                                                                         \
    do {                                                                                                               \
        gDPNoOpTag(dl, PORT_DRAW_TAG | (((object) + 1) << 4) | (kind));                                                \
        port_draw_kind_game(kind, object);                                                                             \
    } while (0)
#else
#define port_draw_tag(dl, kind)
#define port_draw_tag_object(dl, kind, object)
#endif
#endif
