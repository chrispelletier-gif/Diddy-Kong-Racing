/*
 * The software renderer (host/gfx_soft.c) built into the 3DS port under other names, as a reference for comparing
 * the GPU renderer against on identical frames (DKR_DIFF_EVERY, see gfx_rdp.c).
 */
#define gfxb_init refb_init
#define gfxb_start_frame refb_start_frame
#define gfxb_end_frame refb_end_frame
#define gfxb_clear_color refb_clear_color
#define gfxb_clear_depth refb_clear_depth
#define gfxb_draw_triangles refb_draw_triangles
#define gfxb_draw_indexed refb_draw_indexed
#define gfxb_texture_upload refb_texture_upload
#define gfxb_texture_free refb_texture_free
#define gfxb_texture_share refb_texture_share
#define gfxb_gpu_transform refb_gpu_transform
#define gfxb_model_check refb_model_check
#define gfxb_draw_model refb_draw_model
#define gfxb_mesh_create refb_mesh_create
#define gfxb_mesh_free refb_mesh_free
#define gfxb_draw_mesh refb_draw_mesh
#define gfxb_soft_save_png refb_save_png
#define host_frames_rendered ref_frames_rendered
#include <stdlib.h>
// None of the host renderer's own debug settings apply here.
#define getenv(name) ((char *) 0)
#include "../host/gfx_soft.c"
