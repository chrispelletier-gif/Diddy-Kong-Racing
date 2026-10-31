#ifndef PORT_GFX_SPLIT_H
#define PORT_GFX_SPLIT_H

#include "gfx_backend.h"

/*
 * The two-core renderer (roadmap P21, gfx_split.c): the display list walk's backend calls, queued for a second
 * renderer thread while DKR_SPLIT_RENDER is on, else made at once. gfx_split_frame_start starts the frame either
 * way (direct: this frame is made at once whatever the switch says) and says whether its calls are queued.
 */
int gfx_split_frame_start(int direct);
void gfx_split_frame_end(void);
int gfx_split_active(void);
void gfx_split_drain(void);
void gfx_split_clear_color(const short rect[4], unsigned char r, unsigned char g, unsigned char b, unsigned char a);
void gfx_split_clear_depth(const short rect[4]);
void gfx_split_draw_triangles(const GfxDrawState *state, const GfxVertex *verts, int numTriangles);
void gfx_split_draw_indexed(const GfxDrawState *state, const GfxVertex *verts, int numVerts,
                            const unsigned short *indices, int numTriangles);
void gfx_split_texture_upload(GfxTexture *tex);
void gfx_split_texture_share(GfxTexture *dst, const GfxTexture *src);
void gfx_split_texture_free(GfxTexture *tex);

/* Since the last call: the walk's time waiting for the backend, the backend's busy time and frames, and how often the
 * backend was woken (the profiler's "two-core renderer" line). */
void gfx_split_take_times(unsigned long long *walkWaitNs, unsigned long long *backendBusyNs,
                          unsigned int *backendFrames, unsigned int *backendWakes);

#endif
