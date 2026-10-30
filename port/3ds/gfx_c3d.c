/*
 * citro3d (PICA200) rendering backend for the 3DS.
 *
 * Vertices arrive in clip space already (gfx_backend.h); the vertex shader only fits them to the top screen.
 * The RDP colour combiner is compiled into TEV stages, see compile_combiner().
 */
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx_backend.h"
#include "gfx/gfx_split.h"
#include "plat/plat.h"
#include "plat/profile.h"
#include "plat/settings.h"
#include "shader_shbin.h"

/* The display transfer converts the RGBA8 target to the screen's RGB8 and, with anti-aliasing, averages each 2x2 (or
 * 2x1) block of pixels. */
#define DISPLAY_TRANSFER_FLAGS(scaling)                                                                                \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                                 \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |                    \
     GX_TRANSFER_SCALING(scaling))

#define TOP_WIDTH 400
#define TOP_HEIGHT 240
/* The GPU queue is made longer than citro3d's 32 entries, for a 3D frame: two targets to clear and copy. */
#define GX_QUEUE_ENTRIES 64

/* The start of citro3d's context, which is not in its public header: the GPU command queue comes first. */
typedef struct C3DContextStart {
    gxCmdQueue_s gxQueue;
    u32 *cmdBuf;
    size_t cmdBufSize;
    float cmdBufUsage;
    u32 flags; // Which groups of settings C3Di_UpdateContext writes next (citro3d's C3DiF_ bits, below)
} C3DContextStart;
extern C3DContextStart __C3D_Context;
// Where citro3d's own code (C3D_TexBind) finds the flags.
_Static_assert(offsetof(C3DContextStart, flags) == 32, "citro3d's context changed");
/* Bytes of vertices per frame. Each of the two frame slots has its own buffer (see FrameSlot); races draw at most
 * about 3000 triangles, 9000 vertices, a frame, which is 48K of the largest vertices. */
#define VBO_BYTES (48 * 1024 * 48)
/* Bytes of vertex indices per frame (gfxb_draw_indexed, and the meshes' copied by gfxb_draw_meshes): three 16-bit
 * indices a triangle, more for triangles the guard band cuts up. */
#define IBO_BYTES (192 * 1024)

/*
 * What the vertex shader receives (roadmap item P5): the position already in the 3DS screen's clip space, the first
 * texture's coordinates, the colour as four bytes (the shader scales them to 0 to 1) and, only for draws that bind a
 * second texture, its coordinates; without them the shader gets (0, 0) from a fixed attribute. That makes most
 * vertices 28 bytes instead of 48. The formats can change from one draw to the next, see use_vertex_format.
 * DKR_VERTEX_FORMAT=48 writes every vertex with float colours and both coordinates, as before P5 (except that the
 * colours now run from 0 to 255, for the shader).
 */
typedef struct C3DVertex { // VFMT_SHORT
    float pos[4];
    float tc0[2];
    u8 color[4];
} C3DVertex;
typedef struct C3DVertex2 { // VFMT_LONG
    float pos[4];
    float tc0[2];
    u8 color[4];
    float tc1[2];
} C3DVertex2;
typedef struct C3DVertexOld { // VFMT_OLD
    float pos[4];
    float tc0[2];
    float tc1[2];
    float color[4];
} C3DVertexOld;
/* VFMT_MODEL is the game's own vertices for GPU vertex maths (P30, GfxModelVertex): s16 position and texture
 * coordinates and byte colours, 16 bytes; the second coordinates come from the fixed attribute. */
enum { VFMT_SHORT, VFMT_LONG, VFMT_OLD, VFMT_MODEL, VFMT_COUNT };
static const int sVfmtStride[VFMT_COUNT] = { sizeof(C3DVertex), sizeof(C3DVertex2), sizeof(C3DVertexOld),
                                             sizeof(GfxModelVertex) };
_Static_assert(sizeof(GfxModelVertex) == 16, "the model vertex format's attributes must add up to its stride");
static C3D_AttrInfo sVfmtAttr[VFMT_COUNT];
static int sVfmtOld;       // DKR_VERTEX_FORMAT=48
static int sVfmtAttrSet;   // The format of citro3d's attribute setting
static int sVfmtBound = -1; // The format the vertex buffer setting reads, -1 for none yet this frame
static int sVfmtBase;      // Where in sVbo (bytes) the vertex buffer setting starts
static int sVfmtSwitches;   // Format changes in the frame
static const void *sBufMesh; // The mesh arena when the vertex buffer setting points at it (gfxb_draw_meshes), NULL for the frame's

typedef struct C3DTexture {
    C3D_Tex tex;
    float scaleU, scaleV;
    int resample;
    int shared; // tex's memory belongs to another texture (gfxb_texture_share)
    int levels; // Mipmap levels after the first that tex holds (see mip_levels)
    int point;  // Point sampled, so the levels are never used
    int tinted; // The levels are tinted (the "Show" mipmap setting)
    struct C3DTexture *prev, *next; // In sTextures (textures in VRAM, P17)
    int toVram;                     // Waiting to be copied to VRAM (promote_textures)
} C3DTexture;
_Static_assert(offsetof(C3DTexture, tex) == 0, "a C3DTexture's address is its C3D_Tex's (hang_draw_start)");

static C3D_RenderTarget *sTarget;
static C3D_RenderTarget *sTargetRight; // The right eye's, while the screen is in 3D (see the stereo section)
/* Render target size relative to the screen: 2 draws at twice the resolution and downsamples (anti-aliasing).
 * sRenderScale is the vertical one (anti-aliasing), sScaleX the horizontal one, which the 800 pixel mode also sets to
 * 2 (see apply_screen_mode). PLAT_AA_4X doubles both, PLAT_AA_2X only the vertical one: half the GPU's extra work,
 * smoothing the edges closer to horizontal. */
static int sRenderScale = 1, sScaleX = 1;
static int sDefaultAa = PLAT_AA_4X;              // The setting (settings.txt, else DKR_AA).
static volatile int sWantedAa = PLAT_AA_4X;      // plat_set_gpu_config, applied by the renderer between frames.
/* A memory fill of the colour buffer takes a pixel in the buffer's format (RGBA8): black. The 16-bit buffers setting
 * (RGB565 colour, 16-bit depth) was removed on 2026-10-29 because it changed the picture (fog went missing). */
#define CLEAR_BLACK 0x000000FF
static DVLB_s *sShaderDvlb;
static shaderProgram_s sProgram;
static u8 *sVbo; // The vertex buffer of the frame being built
static int sVboUsed; // Bytes
/* Vertices before this one have been flushed from the CPU cache this frame (see flush_new_vertices). */
static int sVboFlushed;
static u16 *sIbo; // The index buffer of the frame being built
static int sIboUsed, sIboFlushed; // Indices
static C3D_Tex sWhiteTex;
extern u32 __ctru_linear_heap, __ctru_linear_heap_size;
static int sFrameStarted;
static int sDepthOnly;
static int sClearing; // draw_clear_rect is drawing
static int sFrameFirstDraw; // No draw yet in this frame's command list
static int sDepthFillPending; // The frame's depth clear is a memory fill (see gfxb_clear_depth)
static int sFrameCount, sFrameTriangles;
/* For the hang report: the size of the last frame handed to the GPU. */
static int sLastFrameTriangles;
static float sLastFrameUsage;
static int sDrawsLeftOut;
/* Command list splits made this frame to let the GPU finish before a blended draw (see drain_before_draw). */
static int sDrainSplits, sLastDrainSplits;

/* The graphics settings, as the Settings menu sets them (any thread) and as the renderer applies them, from the start
 * of each frame: mipmaps on or off (G1, see mip_levels), and the colour boost level (G2, see add_colour_boost). */
static volatile int sMipmapSetting = 1, sColourSetting;
static int sMipmapsUsed = 1, sMipmapsShown, sColourBoost;

/*
 * The top screen (roadmap phase 6). The N64 picture is 320 pixels wide and was shown in the middle of the 400 pixel
 * wide screen.
 *  - Widescreen (G3): the game's camera is widened to 5:3 (plat_aspect_scale), so 3D draws fill the screen; flat ones
 *    (HUD, text, menus) keep their shape in the middle, see gfx_flat_x.
 *  - 800 pixel mode (G4): the screen can show 800x240 when it isn't in 3D (except on the original 2DS). The render
 *    target is twice as wide, which costs the GPU what anti-aliasing costs it; with anti-aliasing on the target is the
 *    same 800x480 as before and only its height is halved, so the extra detail across reaches the screen.
 *  - Stereo 3D (G5): see the stereo section further down.
 * The Settings menu changes them from its thread; the renderer applies them between frames (apply_screen_mode).
 */
#define NARROW (320.0f / 400.0f) // The width of the 4:3 picture on the 400 pixel screen
static volatile int sWideSetting = 1, s800Setting = 1, sStereoSetting = 1;
static int sWide;               // This frame is drawn wide
static float sScale3D = NARROW; // Clip x of 3D draws: NARROW, or 1 for widescreen
/* Edge trimming as this frame draws it (plat_3ds_guard_band_setting, read once a frame: the per-draw path stays off
 * the switch lookup, a string search that costs an Old 3DS instruction-cache refills). */
static int sGuardBandFrame = 1;
int plat_3ds_guard_band_setting(void);
static int sStereo;             // This frame is drawn twice, once for each eye
static int s800;                // The screen shows 800 pixels across
static int sWideUnsupported;    // An original 2DS, which can't show 800 pixels
/* Stereo: the shader moves each vertex across by sEyeSep * (w - sEyeConv) in clip space, in opposite directions for
 * the two eyes, so what is sEyeConv away from the camera stays at the depth of the screen. Flat draws (HUD, text) get
 * w = sEyeConv * sHudW. The player's tuning (roadmap G5a-G5e, "stereo tuning" below) sets all three each frame. */
static float sEyeSep, sEyeConv = 130.0f, sHudW = 1.0f;
static int sEyeUniform = -1; // The shader's eye uniform register, if found
static int sStereoOnce;      // DKR_STEREO_ONCE (P33): stereo frames made once and drawn twice, see stereo_once_lists
static int sOnceFrame;       // This frame is
#define EYE_CMD_WORDS 64
/* citro3d's flags (internal.h C3DiF_*): the target, and the groups of settings a frame made once writes again at its
 * start: attributes, buffers, effect, viewport, scissor, program, combiner buffer and fog, texture status, fog table,
 * textures and combiner stages. */
#define C3D_FLAG_FRAMEBUF (1u << 5)
#define C3D_ONCE_FLAGS ((1u << 2) | (1u << 3) | (1u << 4) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | \
                        (1u << 14) | (1u << 17) | (7u << 23) | (0x3Fu << 26))
void C3Di_DirtyUniforms(GPU_SHADER_TYPE type);

/*
 * The GPU reads vertices and textures from memory, not from the CPU's cache, so whatever the CPU wrote has to be
 * flushed before a command list that uses it is handed over. citro3d's default is to flush the whole linear heap
 * (tens of megabytes) at the end of every frame, and the command buffer split did the same in the middle of one
 * (roadmap item P12). Instead each texture is flushed when it is uploaded and the vertex buffer only from where the
 * last flush stopped; the command list itself is flushed by GX_CMDLIST_FLUSH. DKR_FULL_FLUSH=1 is the old way.
 */
static int full_flush(void) {
    static int sFull = -1;
    if (sFull < 0) {
        sFull = plat_getenv("DKR_FULL_FLUSH") != NULL;
        plat_log("GPU cache flush: %s", sFull ? "whole linear heap" : "written ranges only");
    }
    return sFull;
}

/*
 * GSPGPU_FlushDataCache asks the GSP service to write a range of the CPU's data cache out to memory for the GPU: an
 * IPC round trip to another process each time (the vertices and each command list part every frame, every texture
 * upload). svcFlushProcessDataCache does the same cache maintenance as a plain system call (roadmap item P24).
 * DKR_SVC_FLUSH=1 uses it. Switch bench #176 measured no difference (since P19 there are only a few a frame), so it
 * stays off.
 */
static int sSvcFlush;

static void gpu_flush(const void *data, u32 size) {
    if (sSvcFlush) {
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32) data, size);
    } else {
        GSPGPU_FlushDataCache(data, size);
    }
}

/*
 * Textures in VRAM (roadmap item P17). Every texture used to live in the linear heap in main memory, so the GPU
 * fetched its texels over the bus the CPU uses too; VRAM is the GPU's own. The CPU may not write VRAM (a write there
 * is a data abort, bug report #172), so with DKR_TEX_VRAM=1 a texture is still made and written in the linear heap,
 * then copied to VRAM by the GPU at the start of a later frame (promote_textures, a few a frame so the GPU queue
 * doesn't overflow), while that leaves VRAM_RESERVE free (room for the right eye's target when the 3D slider goes up).
 * Its linear copy is freed once the frames that may have used it are drawn. A render target that later finds no room
 * (anti-aliasing turned up, 3D) has every texture copied back to the linear heap and tries again
 * (evict_vram_textures), so the settings get the VRAM they always did. A texture written again gets new linear
 * memory first, as the CPU writes it (and the "Show" mipmap setting leaves the levels of one in VRAM as they were).
 * Switch bench #176 measured no gain from it (the GPU's time isn't in texel fetches), so it stays off.
 */
#define VRAM_RESERVE (TOP_WIDTH * TOP_HEIGHT * 8)
#define PROMOTE_PER_FRAME 16
static void forget_gpu_state(void);
static void defer_delete(C3DTexture *t);

static int in_vram(const void *data) {
    return (u32) data >= OS_VRAM_VADDR && (u32) data < OS_VRAM_VADDR + OS_VRAM_SIZE;
}
static C3DTexture *sTextures; // Every texture, those sharing another's memory included
static int sToVram;           // How many wait to be copied to VRAM
static u32 sVramTextureBytes;

static void texture_link(C3DTexture *t) {
    t->prev = NULL;
    t->next = sTextures;
    if (sTextures != NULL) {
        sTextures->prev = t;
    }
    sTextures = t;
}

/* Called before a texture is deleted. */
static void texture_unlink(C3DTexture *t) {
    if (t->prev == NULL && sTextures != t) {
        return; // Not in the list.
    }
    if (t->prev != NULL) {
        t->prev->next = t->next;
    } else {
        sTextures = t->next;
    }
    if (t->next != NULL) {
        t->next->prev = t->prev;
    }
    t->prev = t->next = NULL;
    if (t->toVram) {
        t->toVram = 0;
        sToVram--;
    }
    if (!t->shared && in_vram(t->tex.data)) {
        sVramTextureBytes -= C3D_TexCalcTotalSize(t->tex.size, t->levels);
    }
}

/* Points the textures that share from's memory at to. */
static void move_shares(const void *from, void *to) {
    C3DTexture *t;
    for (t = sTextures; t != NULL; t = t->next) {
        if (t->shared && t->tex.data == from) {
            t->tex.data = to;
        }
    }
}

/*
 * Start of a frame: queues GPU copies of a few waiting textures to VRAM. The copies go in the GPU queue ahead of this
 * frame's command list, which already uses the VRAM copy; the frame being drawn now may still read the linear one,
 * which is freed with this frame's other deleted textures.
 */
static void promote_textures(void) {
    C3DTexture *t;
    int done = 0;
    if (sToVram == 0) {
        return;
    }
    for (t = sTextures; t != NULL && sToVram > 0 && done < PROMOTE_PER_FRAME; t = t->next) {
        u32 total;
        void *vram;
        C3DTexture *old;
        if (!t->toVram) {
            continue;
        }
        t->toVram = 0;
        sToVram--;
        total = C3D_TexCalcTotalSize(t->tex.size, t->levels);
        if (vramSpaceFree() < total + VRAM_RESERVE || (old = calloc(1, sizeof(C3DTexture))) == NULL) {
            continue;
        }
        if ((vram = vramAlloc(total)) == NULL) {
            free(old);
            continue;
        }
        GX_TextureCopy((u32 *) t->tex.data, 0, (u32 *) vram, 0, total, GX_TRANSFER_RAW_COPY(1));
        old->tex.data = t->tex.data;
        move_shares(t->tex.data, vram);
        t->tex.data = vram;
        sVramTextureBytes += total;
        defer_delete(old);
        done++;
    }
    if (done > 0) {
        forget_gpu_state(); // The bound textures' addresses changed.
    }
}

/* Moves one texture's memory from VRAM to the linear heap, with the GPU idle and outside a citro3d frame. */
PLAT_COLD static int texture_to_linear(C3DTexture *t) {
    u32 total = C3D_TexCalcTotalSize(t->tex.size, t->levels);
    void *data = linearAlloc(total);
    if (data == NULL) {
        return 0;
    }
    gpu_flush(data, total); // No dirty cache lines left to land on the copy later.
    C3D_SyncTextureCopy((u32 *) t->tex.data, 0, (u32 *) data, 0, total, GX_TRANSFER_RAW_COPY(1));
    move_shares(t->tex.data, data);
    vramFree(t->tex.data);
    t->tex.data = data;
    sVramTextureBytes -= total;
    return 1;
}

/*
 * With the GPU idle: moves every texture in VRAM to the linear heap, for a render target that needs the room.
 * Returns how many moved.
 */
PLAT_COLD static int evict_vram_textures(void) {
    C3DTexture *t;
    int moved = 0, count = 0;
    for (t = sTextures; t != NULL; t = t->next) {
        if (t->toVram) {
            t->toVram = 0; // They would only take the room back.
            sToVram--;
        }
        if (!t->shared && in_vram(t->tex.data)) {
            count++;
            moved += texture_to_linear(t);
        }
    }
    if (count > 0) {
        plat_log("textures: moved %d of %d from VRAM to main memory for a render target (%lu KB left in VRAM)",
                 moved, count, (unsigned long) (sVramTextureBytes / 1024));
        forget_gpu_state();
    }
    return moved;
}

static void flush_new_vertices(void) {
    if (sVboFlushed > sVboUsed) {
        sVboFlushed = 0; // The buffer was started again.
    }
    if (sVboUsed > sVboFlushed) {
        gpu_flush(sVbo + sVboFlushed, sVboUsed - sVboFlushed);
    }
    sVboFlushed = sVboUsed;
    if (sIboFlushed > sIboUsed) {
        sIboFlushed = 0;
    }
    if (sIboUsed > sIboFlushed && sIbo != NULL) {
        gpu_flush(sIbo + sIboFlushed, (sIboUsed - sIboFlushed) * sizeof(u16));
    }
    sIboFlushed = sIboUsed;
}


/*
 * Each citro3d setter marks its group of GPU settings for writing to the command list, even when the value does not
 * change, and a batch used to call all of them: six combiner stages, blending, depth, culling, scissor, fog colour
 * and both textures, although most batches only change their texture. So the settings last passed to citro3d are
 * kept here and a setter is only called when its value changes (roadmap item P4). This mirrors citro3d's own copy
 * of the settings, not the GPU's registers, so citro3d still rewrites everything itself after the HOME menu.
 * Forgotten at the start of each frame (binding the render target resets the scissor) and when a texture is
 * uploaded (its settings may change in place). DKR_STATE_CACHE=0 sets everything for every batch, as before.
 */
static struct {
    int known; // The fields below hold what citro3d has.
    C3D_Tex *tex[2];
    C3D_TexEnv texEnv[6];
    u8 texEnvBuf; // Stages that write the combiner buffer
    u32 fogColor;
    u8 blend;
    u8 alphaTest, alphaRef;
    u8 depthEnable, depthFunc, depthMask;
    u8 earlyDepth;
    u8 decal;
    u8 cull;
    short scissor[4];
    u8 scissorFlat;
} sGpu;

PLAT_HOT static void forget_gpu_state(void) {
    sGpu.known = 0;
}

/******************************** Setup ********************************/

/*
 * Vertices arrive in clip space for the N64 screen. To fit them to the 3DS top screen (in gfxb_draw_triangles):
 *  - x is scaled into the middle of the screen, or to fill it in widescreen,
 *  - z is remapped from [-w, w] (near to far) to [-w, 0] with depth = -z / w, so nearer is larger; the N64 has no far
 *    plane, so anything beyond it is pinned to the farthest depth instead of being clipped,
 *  - then everything is rotated a quarter turn for the 3DS screen: x' = y, y' = -x.
 */
static inline void to_screen_scaled(const GfxVertex *v, float *pos, float scale3D) {
    float z = 0.5f * (v->z - v->w);
    pos[0] = v->y;
    pos[1] = -v->x * scale3D;
    pos[2] = z < 0.0f ? z : 0.0f;
    pos[3] = v->w;
}

static inline void to_screen(const GfxVertex *v, float *pos) {
    to_screen_scaled(v, pos, sScale3D);
}

/* The same for a flat draw: its 4:3 shape kept on a wide screen (gfx_flat_x), and in stereo its w made sEyeConv, so
 * that it sits at the depth of the screen, or nearer or farther by the HUD depth setting (scaling the whole position
 * leaves where it lands unchanged). */
static inline void to_screen_flat(const GfxVertex *v, float *pos, int flat) {
    float z = 0.5f * (v->z - v->w);
    float f = (sStereo && v->w > 0.0f) ? sEyeConv * sHudW / v->w : 1.0f;
    pos[0] = v->y * f;
    pos[1] = -(sWide ? gfx_flat_x(v->x, v->w, NARROW, flat) : v->x * NARROW) * f;
    pos[2] = (z < 0.0f ? z : 0.0f) * f;
    pos[3] = v->w * f;
}

static inline void place(const GfxVertex *v, float *pos, int flat) {
    if (flat) {
        to_screen_flat(v, pos, flat);
    } else {
        to_screen(v, pos);
    }
}

/* The attribute settings of the vertex formats. The shader's inputs are v0 position, v1 and v2 the texture
 * coordinates, v3 colour; the new formats list them in the same order so that only the last one differs. */
PLAT_COLD static void init_vertex_formats(void) {
    C3D_AttrInfo *attr;
    int i;
    sVfmtOld = plat_getenv("DKR_VERTEX_FORMAT") != NULL && atoi(plat_getenv("DKR_VERTEX_FORMAT")) == 48;
    for (i = 0; i < VFMT_COUNT; i++) {
        attr = &sVfmtAttr[i];
        AttrInfo_Init(attr);
        if (i == VFMT_MODEL) {
            AttrInfo_AddLoader(attr, 0, GPU_SHORT, 4);
            AttrInfo_AddLoader(attr, 1, GPU_SHORT, 2);
            AttrInfo_AddLoader(attr, 3, GPU_UNSIGNED_BYTE, 4);
            AttrInfo_AddFixed(attr, 2);
            continue;
        }
        AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);
        AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
        if (i == VFMT_OLD) {
            AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 2);
            AttrInfo_AddLoader(attr, 3, GPU_FLOAT, 4);
        } else {
            AttrInfo_AddLoader(attr, 3, GPU_UNSIGNED_BYTE, 4);
            if (i == VFMT_LONG) {
                AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 2);
            } else {
                AttrInfo_AddFixed(attr, 2);
            }
        }
    }
    sVfmtAttrSet = sVfmtOld ? VFMT_OLD : VFMT_SHORT;
    C3D_SetAttrInfo(&sVfmtAttr[sVfmtAttrSet]);
    // Attribute 3 of the short and model formats: v2. The old format loads that attribute, so it doesn't mind.
    C3D_FixedAttribSet(3, 0.0f, 0.0f, 0.0f, 0.0f);
    plat_log("vertex format: %s", sVfmtOld ? "48 bytes (DKR_VERTEX_FORMAT=48)" : "28 bytes, 36 with a second texture");
}

/*
 * Makes the GPU read the vertices from sVboUsed on in format fmt, and returns the index of the first. A change of
 * format starts the buffer setting at sVboUsed (rounded up to 16 bytes) with the new stride; the same format carries
 * on from the last draw's vertices, so the settings stay as they are.
 */
/* The new buffer setting (and attribute setting) of a format change: out of line, as most draws keep the format. */
static void __attribute__((noinline)) bind_vertex_format(int fmt) {
    C3D_BufInfo *buf = C3D_GetBufInfo();
    sVboUsed = (sVboUsed + 15) & ~15;
    sVfmtBase = sVboUsed;
    if (fmt != sVfmtAttrSet) {
        C3D_SetAttrInfo(&sVfmtAttr[fmt]);
        sVfmtAttrSet = fmt;
    }
    BufInfo_Init(buf);
    BufInfo_Add(buf, sVbo + sVfmtBase, sVfmtStride[fmt], fmt == VFMT_SHORT || fmt == VFMT_MODEL ? 3 : 4,
                fmt == VFMT_SHORT || fmt == VFMT_MODEL ? 0x210 : 0x3210);
    sBufMesh = NULL;
    if (sVfmtBound >= 0) {
        sVfmtSwitches++;
    }
    sVfmtBound = fmt;
}

PLAT_HOT static int use_vertex_format(int fmt) {
    if (fmt != sVfmtBound) {
        bind_vertex_format(fmt);
    }
    // Divided by a constant for each format: the ARM11 has no divide instruction, and this is on the per-draw path.
    return fmt == VFMT_SHORT   ? (sVboUsed - sVfmtBase) / (int) sizeof(C3DVertex)
           : fmt == VFMT_LONG  ? (sVboUsed - sVfmtBase) / (int) sizeof(C3DVertex2)
           : fmt == VFMT_MODEL ? (sVboUsed - sVfmtBase) / (int) sizeof(GfxModelVertex)
                               : (sVboUsed - sVfmtBase) / (int) sizeof(C3DVertexOld);
}

/*
 * GPU self test, for finding what hangs a real 3DS when an emulator copes. Before the game starts, a few frames are
 * drawn that each add one feature of the renderer to the one before, and the bottom screen says which one never
 * finished. Once it has passed, gpu_test.txt in the data directory remembers that and later starts skip it, until
 * GPU_SELF_TEST_VERSION changes. DKR_GPU_TEST=1 runs it anyway and DKR_NO_GPU_TEST=1 never runs it.
 */

// Bump when the self test, the vertex format or the shader changes, so that the test runs again once.
#define GPU_SELF_TEST_VERSION 4
static void draw_clear_rect(const short rect[4], u32 rgba, int depthOnly);
static void apply_combiner(const GfxDrawState *state);
static void apply_fog(const GfxDrawState *state);
static void write_eye(float mul, float add);
static void hand_over_draw_parts(void);
static void init_transform(void);
static void transform_frame_drawn(int frame);
/* citro3d's own function that writes the changed settings to the command list (not in its public header). */
void C3Di_UpdateContext(void);

static int sGpuTestStep;

static void gpu_test_wait(const char *what) {
    unsigned long long start = plat_time_ns();
    while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        if (plat_time_ns() - start > 2000000000ULL) {
            plat_status("GPU test %d HUNG: %s", sGpuTestStep, what);
            gPlatGfxStage = "GPU test hung";
            for (;;) {
                plat_sleep_ns(1000000000ULL);
            }
        }
        plat_sleep_ns(250000);
    }
    C3D_FrameEnd(0); // Nothing in it: only used to wait for the GPU.
}

/* Writes a white triangle at sVboUsed in format fmt (the game's short one if -1); returns its first vertex. */
static int gpu_test_triangle(int fmt) {
    static const float xs[3] = { -0.5f, 0.5f, 0.0f }, ys[3] = { -0.5f, -0.5f, 0.5f };
    int i, first;
    if (fmt < 0) {
        fmt = sVfmtOld ? VFMT_OLD : VFMT_SHORT;
    }
    first = use_vertex_format(fmt);
    for (i = 0; i < 3; i++) {
        u8 *v = sVbo + sVboUsed + i * sVfmtStride[fmt];
        float *pos = (float *) v;
        memset(v, 0, sVfmtStride[fmt]);
        pos[0] = xs[i];
        pos[1] = ys[i];
        pos[3] = 1.0f;
        if (fmt == VFMT_OLD) {
            float *color = ((C3DVertexOld *) v)->color;
            color[0] = color[1] = color[2] = color[3] = 255.0f;
        } else {
            memset(((C3DVertex *) v)->color, 255, 4);
        }
    }
    sVboUsed += 3 * sVfmtStride[fmt];
    flush_new_vertices();
    return first;
}

static void gpu_test_step(const char *what) {
    sGpuTestStep++;
    plat_log("GPU test %d: %s", sGpuTestStep, what);
    gpu_test_wait(what);
    sVboUsed = 0;
    sVfmtBound = -1;
    C3D_FrameBegin(0);
    C3D_RenderTargetClear(sTarget, C3D_CLEAR_ALL, CLEAR_BLACK, 0);
    C3D_FrameDrawOn(sTarget);
    write_eye(0.0f, 0.0f);
}

static void gpu_test_end(const char *what) {
    C3D_FrameEnd(0);
    gpu_test_wait(what);
}

static void gpu_test_marker_path(char *out, size_t size) {
    snprintf(out, size, "%s/gpu_test.txt", plat_data_dir());
}

static int gpu_test_passed_before(void) {
    char path[256], text[32];
    int got;
    gpu_test_marker_path(path, sizeof(path));
    got = plat_load_file_start(path, text, sizeof(text) - 1);
    if (got <= 0) {
        return 0;
    }
    text[got] = '\0';
    return strncmp(text, "passed ", 7) == 0 && atoi(text + 7) == GPU_SELF_TEST_VERSION;
}

static void gpu_test_mark_passed(void) {
    char path[256], text[32];
    gpu_test_marker_path(path, sizeof(path));
    snprintf(text, sizeof(text), "passed %d\n", GPU_SELF_TEST_VERSION);
    plat_save_file(path, text, strlen(text));
}

PLAT_COLD static void gpu_self_test(void) {
    static const char *const sNames[] = {
        "empty frame",
        "triangle",
        "+ texture unit 1",
        "+ fog unit off",
        "+ scissor",
        "+ depth test",
        "+ game combiner",
        "+ fog table",
        "+ second texture coordinates",
        "+ vertex format changes",
        "game clear rectangles",
        "+ stereo eye offset",
        "+ indexed triangles",
    };
    GfxDrawState state;
    C3D_TexEnv *env;
    int i, first;

    if (plat_getenv("DKR_NO_GPU_TEST") != NULL) {
        return;
    }
    if (plat_getenv("DKR_GPU_TEST") == NULL && gpu_test_passed_before()) {
        plat_log("GPU test skipped: passed before (gpu_test.txt)");
        return;
    }
    plat_status("GPU test...");
    gPlatGfxStage = "GPU test";

    // 1: an empty frame, like the game's first one.
    gpu_test_step(sNames[0]);
    gpu_test_end(sNames[0]);

    // 2: one flat triangle with the simplest state.
    gpu_test_step(sNames[1]);
    first = gpu_test_triangle(-1);
    C3D_TexBind(0, &sWhiteTex);
    env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    for (i = 1; i < 6; i++) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_DrawArrays(GPU_TRIANGLES, first, 3);
    gpu_test_end(sNames[1]);

    // 3 to 8: add the renderer's features one at a time.
    gpu_test_step(sNames[2]);
    first = gpu_test_triangle(-1);
    C3D_TexBind(1, &sWhiteTex);
    C3D_DrawArrays(GPU_TRIANGLES, first, 3);
    gpu_test_end(sNames[2]);

    gpu_test_step(sNames[3]);
    first = gpu_test_triangle(-1);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_DrawArrays(GPU_TRIANGLES, first, 3);
    gpu_test_end(sNames[3]);

    gpu_test_step(sNames[4]);
    first = gpu_test_triangle(-1);
    C3D_SetScissor(GPU_SCISSOR_NORMAL, 40, 40, 200, 360);
    C3D_DrawArrays(GPU_TRIANGLES, first, 3);
    gpu_test_end(sNames[4]);

    gpu_test_step(sNames[5]);
    first = gpu_test_triangle(-1);
    C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL);
    C3D_DepthMap(true, -1.0f, 0.0002f);
    C3D_DrawArrays(GPU_TRIANGLES, first, 3);
    gpu_test_end(sNames[5]);

    memset(&state, 0, sizeof(state));
    state.combiner.numCycles = 1;
    state.combiner.rgb[0].a = GFX_CC_TEXEL0;
    state.combiner.rgb[0].b = GFX_CC_ZERO;
    state.combiner.rgb[0].c = GFX_CC_SHADE;
    state.combiner.rgb[0].d = GFX_CC_ZERO;
    state.combiner.alpha[0] = state.combiner.rgb[0];
    state.prim[0] = state.prim[1] = state.prim[2] = state.prim[3] = 255;

    gpu_test_step(sNames[6]);
    first = gpu_test_triangle(-1);
    apply_combiner(&state);
    C3D_DrawArrays(GPU_TRIANGLES, first, 3);
    gpu_test_end(sNames[6]);

    gpu_test_step(sNames[7]);
    first = gpu_test_triangle(-1);
    state.fogEnabled = 1;
    state.fogMul = 256;
    state.fogOffset = -100;
    apply_fog(&state);
    C3D_DrawArrays(GPU_TRIANGLES, first, 3);
    state.fogEnabled = 0;
    apply_fog(&state);
    gpu_test_end(sNames[7]);

    // 9 and 10: the vertex format with both texture coordinates, then changes of format within a frame (P5).
    gpu_test_step(sNames[8]);
    first = gpu_test_triangle(sVfmtOld ? VFMT_OLD : VFMT_LONG);
    C3D_DrawArrays(GPU_TRIANGLES, first, 3);
    gpu_test_end(sNames[8]);

    gpu_test_step(sNames[9]);
    for (i = 0; i < 4; i++) {
        first = gpu_test_triangle(sVfmtOld ? VFMT_OLD : (i & 1) ? VFMT_LONG : VFMT_SHORT);
        C3D_DrawArrays(GPU_TRIANGLES, first, 3);
    }
    gpu_test_end(sNames[9]);

    gpu_test_step(sNames[10]);
    {
        static const short sFull[4] = { 0, 0, GFX_SCREEN_WIDTH, GFX_SCREEN_HEIGHT };
        sVboUsed = 0;
        draw_clear_rect(sFull, 0x204080FF, 0);
        draw_clear_rect(sFull, 0, 1);
        sVboUsed = 0;
        sFrameTriangles = 0;
    }
    gpu_test_end(sNames[10]);

    // 12: the shader's stereo offset (G5) moving the triangle across.
    gpu_test_step(sNames[11]);
    first = gpu_test_triangle(-1);
    write_eye(0.05f, -0.05f);
    C3D_DrawArrays(GPU_TRIANGLES, first, 3);
    write_eye(0.0f, 0.0f);
    gpu_test_end(sNames[11]);

    // 14: vertices drawn by index (P16), the index buffer in linear memory next to the vertices.
    gpu_test_step(sNames[12]);
    first = gpu_test_triangle(-1);
    {
        static const u16 sOrder[3] = { 2, 0, 1 };
        for (i = 0; i < 3; i++) {
            sIbo[i] = (u16) (first + sOrder[i]);
        }
        gpu_flush(sIbo, 3 * sizeof(u16));
        C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, sIbo);
    }
    gpu_test_end(sNames[12]);
    sVfmtBound = -1;

    plat_status("GPU test passed");
    gpu_test_mark_passed();
}

static const char *aa_name(int aa) {
    return aa == PLAT_AA_4X ? "4x" : aa == PLAT_AA_2X ? "2x" : "off";
}

/* A render target: RGBA8 colour, 24-bit depth and stencil. */
static C3D_RenderTarget *new_target(int width, int height) {
    C3D_RenderTarget *target = NULL;
    int tries;
    // Textures in VRAM (P17) give way to a target: the second try is with them moved out.
    for (tries = 0; tries < 2 && target == NULL; tries++) {
        if (tries == 1 && evict_vram_textures() == 0) {
            break;
        }
        target = C3D_RenderTargetCreate(width, height, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    }
    return target;
}

/*
 * The render target for the top screen: twice the screen's resolution down for anti-aliasing (scale), and across for
 * 4x anti-aliasing or the 800 pixel mode (across). The doubled one needs 3MB of VRAM, so fall back if it does not fit.
 */
static void create_target(int scale, int across) {
    sTarget = NULL;
    if (scale == 2 || across == 2) {
        sTarget = new_target(TOP_HEIGHT * scale, TOP_WIDTH * across);
        sRenderScale = scale;
        sScaleX = across;
    }
    if (sTarget == NULL) {
        sTarget = new_target(TOP_HEIGHT, TOP_WIDTH);
        sRenderScale = 1;
        sScaleX = 1;
    }
}

/*
 * How the display transfer copies the target to the screen: anti-aliased targets are halved down, and across too
 * unless the screen shows 800 pixels. In 3D, sTarget is the left eye's and sTargetRight the right eye's.
 */
static u32 sTransferFlags;
static C3D_RenderTarget *sLinked; // The target the top screen shows (link_target)

static void link_target(void) {
    u32 scaling = GX_TRANSFER_SCALE_NO;
    if (sRenderScale == 2) {
        scaling = sScaleX == 2 && !s800 ? GX_TRANSFER_SCALE_XY : GX_TRANSFER_SCALE_X;
    }
    sTransferFlags = DISPLAY_TRANSFER_FLAGS(scaling);
    C3D_RenderTargetSetOutput(sTarget, GFX_TOP, GFX_LEFT, sTransferFlags);
    sLinked = sTarget;
    if (sStereo && sTargetRight != NULL) {
        C3D_RenderTargetSetOutput(sTargetRight, GFX_TOP, GFX_RIGHT, sTransferFlags);
    } else {
        C3D_RenderTargetSetOutput(NULL, GFX_TOP, GFX_RIGHT, 0);
    }
}

/*
 * Adaptive anti-aliasing (DKR_ADAPTIVE_AA, default off; shelved after switch bench #246, where it never switched): with anti-aliasing on, a frame the GPU took nearly a whole
 * frame's time to draw (over 92% of it, see adaptive_observe) sends the next ones to a target at the screen's own
 * resolution (sLowTarget, made when first needed) for sAdaptHoldLen frames; the first anti-aliased frame after that
 * is a probe, and if it is slow again the hold doubles (up to ADAPT_HOLD_MAX), so a heavy stretch of track stays at
 * the low resolution instead of flickering between the two. Never in 3D (which has no anti-aliasing) or while a
 * snapshot is taken. The main target keeps its sizes in sMainScale and sMainScaleX while the low one is in use.
 * Switching targets needs no wait for the GPU: the screen is linked to the frame's target just before it is handed
 * over (adaptive_link, inside citro3d's frame). DKR_ADAPTIVE_AA=2 counts every anti-aliased frame as slow, to see the
 * switching itself (each hold, then one probe frame).
 */
#define ADAPT_HOLD 60
#define ADAPT_HOLD_MAX 960
#define ADAPT_PROBE 30
static int sAdaptive;
static C3D_RenderTarget *sLowTarget, *sMainTarget;
static int sMainScale, sMainScaleX, sLowActive, sLowFresh, sLowFailed;
static int sAdaptHold, sAdaptHoldLen = ADAPT_HOLD, sAdaptProbe, sAdaptLastFrame = -1;
static int sAdaptSeen, sAdaptLow, sAdaptSlow;

/* Back to the main target (the frame's choice is made again in adaptive_pick). */
static void adaptive_restore_main(void) {
    if (sLowActive) {
        sTarget = sMainTarget;
        sRenderScale = sMainScale;
        sScaleX = sMainScaleX;
        sLowActive = 0;
    }
}

/* With the GPU idle and not in a citro3d frame (apply_screen_mode): the low target is made again for the new mode. */
static void adaptive_delete_low(void) {
    if (sLowTarget != NULL) {
        C3D_RenderTargetDelete(sLowTarget);
        sLowTarget = NULL;
    }
    sLowFailed = 0;
}

/* From any thread (the GPU benchmark): anti-aliasing (PLAT_AA_*) for the frames from the next one; -1 is the player's
 * setting. */
void plat_set_gpu_config(int aa) {
    sWantedAa = aa < 0 ? sDefaultAa : aa;
}

/* The Settings menu: the player's anti-aliasing choice (PLAT_AA_*), saved, and used from the next frame. */
void plat_3ds_set_antialias_setting(int aa) {
    sDefaultAa = aa < PLAT_AA_OFF || aa > PLAT_AA_2X ? PLAT_AA_OFF : aa;
    sWantedAa = sDefaultAa;
    settings_set("aa", sDefaultAa);
}

int plat_3ds_antialias_setting(void) {
    return sDefaultAa;
}

/* The Settings menu: the colour boost level (0 off, 1 to 3 stronger; see add_colour_boost), from the next frame. */
void plat_3ds_set_colour_setting(int level) {
    sColourSetting = level < 0 ? 0 : level > 3 ? 3 : level;
    settings_set("colour", sColourSetting);
}

int plat_3ds_colour_setting(void) {
    return sColourSetting;
}

/* The anti-aliasing (PLAT_AA_*) of the frames being drawn now. With 800 pixels across, either is 2x. */
int plat_antialias(void) {
    return sRenderScale != 2 ? PLAT_AA_OFF : sScaleX == 2 && !s800 ? PLAT_AA_4X : PLAT_AA_2X;
}

/* The Settings menu's screen page (phase 6, see sWideSetting): each is saved and used from the next frame. */
void plat_3ds_set_screen_setting(int which, int on) {
    static const char *const sKeys[3] = { "widescreen", "wide800", "stereo3d" };
    volatile int *settings[3] = { &sWideSetting, &s800Setting, &sStereoSetting };
    if (which >= 0 && which < 3) {
        // Widescreen is 0 off, or its style: 1 Wide, 2 Balanced, 3 Zoomed (plat_camera_fov). The others are on or off.
        int value = which == 0 ? (on < 0 || on > 3 ? 0 : on) : on != 0;
        *settings[which] = value;
        settings_set(sKeys[which], value);
    }
}

int plat_3ds_screen_setting(int which) {
    return which == 0 ? sWideSetting : which == 1 ? s800Setting : which == 2 ? sStereoSetting : 0;
}

/* Whether the 800 pixel mode can be shown (not on an original 2DS). */
int plat_3ds_wide800_supported(void) {
    return !sWideUnsupported;
}

/* The widescreen setting, unless the switch bench sets its own (BENCH_WIDESCREEN). */
static int wide_setting(void) {
    return plat_switch("BENCH_WIDESCREEN", sWideSetting);
}

/* The widescreen style as frames use it: read once a frame (apply_screen_mode), as the renderer asks for it at every
 * billboard load and the switch lookup is a string search; -1 until the first frame. */
static volatile int sWideStyleFrame = -1;

static int wide_style(void) {
    int style = sWideStyleFrame;
    return style >= 0 ? style : wide_setting();
}

float plat_aspect_scale(void) {
    return wide_style() ? (float) TOP_WIDTH / GFX_SCREEN_WIDTH : 1.0f;
}

float plat_fov_tan_scale(void) {
    // Wide 1, Balanced 1 / sqrt(1.25), Zoomed 1 / 1.25 (the 4:3 picture's sides).
    static const float sTanScale[4] = { 1.0f, 1.0f, 0.894427f, 0.8f };
    int style = wide_style();
    return style >= 0 && style <= 3 ? sTanScale[style] : 1.0f;
}

float plat_camera_fov(float fovy) {
    float scale = plat_fov_tan_scale();
    if (scale == 1.0f) {
        return fovy;
    }
    // Half the angle in radians, and the whole angle back in degrees.
    return 2.0f * atanf(tanf(fovy * (3.14159265f / 360.0f)) * scale) * (180.0f / 3.14159265f);
}

/*
 * The stereo depth: how far apart the eyes' pictures are, as a fraction of the screen's half width, for the farthest
 * things with the 3D slider all the way up (DKR_STEREO_SEP, in thousandths), and the distance from the camera that
 * stays at the screen's depth (DKR_STEREO_CONV, in the game's units: the camera follows the kart about 130 behind).
 * The player's 3D depth and convergence settings scale these.
 */
static float sBaseConv = 130.0f;

static float stereo_max_sep(void) {
    static float sSep = -1.0f;
    if (sSep < 0.0f) {
        const char *env = plat_getenv("DKR_STEREO_SEP");
        const char *conv = plat_getenv("DKR_STEREO_CONV");
        sSep = (env != NULL ? atoi(env) : 25) / 1000.0f;
        if (conv != NULL && atoi(conv) > 0) {
            sBaseConv = (float) atoi(conv);
        }
        plat_log("stereo: separation %.3f, screen depth at %.0f", sSep, sBaseConv);
    }
    return sSep;
}

/******************************** Stereo tuning ********************************/

/*
 * The 3D settings on the Screen page (roadmap G5a-G5e, after the Donkey Kong 64 3DS port's): 3D depth (percent of the
 * usual separation), convergence (percent of the usual screen depth: higher brings the kart and what is near it out
 * of the screen), HUD depth (steps out of the screen, or into it when negative), auto convergence and its comfort
 * target. Saved in settings.txt, changed from the menu's thread and read by the renderer between frames.
 */
enum { STEREO_DEPTH, STEREO_CONV, STEREO_HUD, STEREO_AUTO, STEREO_COMFORT, STEREO_TUNING_COUNT };
static const struct {
    const char *key;
    int lo, hi, step, usual;
} sTuningRange[STEREO_TUNING_COUNT] = {
    { "stereo_depth", 25, 200, 25, 100 }, { "stereo_conv", 50, 200, 10, 100 },  { "stereo_hud", -5, 5, 1, 0 },
    { "stereo_auto", 0, 1, 1, 1 },        { "stereo_comfort", 0, 100, 25, 50 },
};
static volatile int sTuning[STEREO_TUNING_COUNT];

static void read_stereo_tuning(void) {
    int i, v;
    for (i = 0; i < STEREO_TUNING_COUNT; i++) {
        v = settings_get(sTuningRange[i].key, sTuningRange[i].usual);
        sTuning[i] = v < sTuningRange[i].lo || v > sTuningRange[i].hi ? sTuningRange[i].usual : v;
    }
    plat_log("stereo tuning: depth %d%%, convergence %d%%, HUD %d, auto convergence %d, comfort %d%%",
             sTuning[STEREO_DEPTH], sTuning[STEREO_CONV], sTuning[STEREO_HUD], sTuning[STEREO_AUTO],
             sTuning[STEREO_COMFORT]);
}

int plat_3ds_stereo_tuning(int which) {
    return which >= 0 && which < STEREO_TUNING_COUNT ? sTuning[which] : 0;
}

/* Steps a setting by step, wrapping round at either end, and saves it. */
void plat_3ds_step_stereo_tuning(int which, int step) {
    int v;
    if (which < 0 || which >= STEREO_TUNING_COUNT) {
        return;
    }
    v = sTuning[which] + (step < 0 ? -1 : 1) * sTuningRange[which].step;
    v = v > sTuningRange[which].hi ? sTuningRange[which].lo : v < sTuningRange[which].lo ? sTuningRange[which].hi : v;
    sTuning[which] = v;
    settings_set(sTuningRange[which].key, v);
}

/*
 * Auto convergence (G5d/G5e). While a frame is drawn in 3D, near_depth_note keeps the nearest vertex of the 3D draws in
 * each patch of a grid over the screen (leaving out its bottom quarter, where the track under the kart is always
 * close, and the very edges). Between frames, near_depth_take makes one value of them: the second nearest patch, so
 * one small thing (a banana, a spark) is not the whole story, or none when fewer than three patches had anything (a
 * sky). auto_convergence then smooths it, quick to follow something coming close and slow to let go, jumping at a
 * camera cut, and brings the screen's depth near enough that the nearest thing comes out of the screen by at most the
 * comfort target: a percentage of how far the farthest things go into it. The vertices are already transformed on the
 * CPU, so nothing has to be read back from the GPU.
 */
#define NEAR_COLS 8
#define NEAR_ROWS 4
static float sNear[NEAR_COLS * NEAR_ROWS];

static void near_depth_clear(void) {
    int i;
    for (i = 0; i < NEAR_COLS * NEAR_ROWS; i++) {
        sNear[i] = 0.0f;
    }
}

static void near_depth_note(const GfxVertex *verts, int numVerts) {
    int i, col, row;
    for (i = 0; i < numVerts; i++) {
        const GfxVertex *v = &verts[i];
        float w = v->w, x, y;
        if (!(w > 1.0f)) {
            continue;
        }
        // Over x -0.9..0.9 and y -0.5..0.9 of the screen (+y up).
        x = (v->x / w + 0.9f) * (NEAR_COLS / 1.8f);
        y = (v->y / w + 0.5f) * (NEAR_ROWS / 1.4f);
        if (!(x >= 0.0f && x < NEAR_COLS && y >= 0.0f && y < NEAR_ROWS)) {
            continue;
        }
        col = (int) x;
        row = (int) y;
        if (sNear[row * NEAR_COLS + col] == 0.0f || w < sNear[row * NEAR_COLS + col]) {
            sNear[row * NEAR_COLS + col] = w;
        }
    }
}

static float near_depth_take(void) {
    float nears[NEAR_COLS * NEAR_ROWS], t;
    int i, j, n = 0;
    for (i = 0; i < NEAR_COLS * NEAR_ROWS; i++) {
        if (sNear[i] > 0.0f) {
            // Kept in order, nearest first.
            for (j = n++; j > 0 && nears[j - 1] > sNear[i]; j--) {
                nears[j] = nears[j - 1];
            }
            nears[j] = sNear[i];
        }
    }
    near_depth_clear();
    if (n < 3) {
        return -1.0f;
    }
    t = nears[1];
    return t;
}

static struct {
    float history[5];
    int count, cursor, cutFrames;
    float near;    // The smoothed nearest distance
    float invConv; // 1 / the screen's depth, eased
    float last;
} sAuto;

static void auto_convergence_reset(void) {
    memset(&sAuto, 0, sizeof(sAuto));
}

static float auto_convergence(float nearest, float manual, float comfort) {
    float sorted[5], median, target;
    int i, j;

    if (nearest <= 0.0f) {
        // Nothing to go by this frame: keep the last.
        return sAuto.last > 0.0f && sAuto.last < manual ? sAuto.last : manual;
    }
    sAuto.history[sAuto.cursor] = nearest;
    sAuto.cursor = (sAuto.cursor + 1) % 5;
    sAuto.count = sAuto.count < 5 ? sAuto.count + 1 : 5;
    for (i = 0; i < sAuto.count; i++) {
        float h = sAuto.history[(sAuto.cursor + 4 - i) % 5];
        for (j = i; j > 0 && sorted[j - 1] > h; j--) {
            sorted[j] = sorted[j - 1];
        }
        sorted[j] = h;
    }
    median = sorted[sAuto.count / 2];
    if (sAuto.near <= 0.0f) {
        sAuto.near = median;
    } else {
        // A camera cut stays changed for a second frame, a flicker does not: then jump straight to it.
        sAuto.cutFrames = nearest * 2.0f < sAuto.near ? sAuto.cutFrames + 1 : 0;
        if (sAuto.cutFrames >= 2) {
            sAuto.near = nearest;
            sAuto.invConv = 0.0f;
            sAuto.cutFrames = 0;
        } else {
            sAuto.near += (median - sAuto.near) * (median < sAuto.near ? 0.3f : 0.05f);
        }
    }
    // What is at distance d comes out of the screen by sep * (conv / d - 1); the farthest go in by sep.
    target = sAuto.near * (1.0f + comfort);
    target = target < manual ? target : manual;
    target = target > sAuto.near * 0.25f ? target : sAuto.near * 0.25f;
    target = target > 10.0f ? target : 10.0f;
    if (sAuto.invConv <= 0.0f) {
        sAuto.invConv = 1.0f / target;
    } else {
        sAuto.invConv += (1.0f / target - sAuto.invConv) * (1.0f / target > sAuto.invConv ? 0.25f : 0.04f);
    }
    sAuto.last = 1.0f / sAuto.invConv;
    return sAuto.last < manual ? sAuto.last : manual;
}

/* Between frames: the separation for the slider, the screen's depth and the HUD's for the coming frame. */
static void apply_stereo_tuning(int stereo, float slider) {
    float manual;
    sEyeSep = stereo ? slider * stereo_max_sep() * sTuning[STEREO_DEPTH] / 100.0f : 0.0f;
    manual = sBaseConv * sTuning[STEREO_CONV] / 100.0f;
    if (stereo && sTuning[STEREO_AUTO]) {
        static float sLogged;
        float nearest = near_depth_take();
        sEyeConv = auto_convergence(nearest, manual, sTuning[STEREO_COMFORT] / 100.0f);
        if (sEyeConv < sLogged * 0.8f || sEyeConv > sLogged * 1.25f) {
            // Only when it moves a good way, so the log shows what it does without a line every frame.
            plat_log("stereo: auto convergence %.0f of %.0f (nearest %.0f)", sEyeConv, manual, nearest);
            sLogged = sEyeConv;
        }
    } else {
        near_depth_clear();
        auto_convergence_reset();
        sEyeConv = manual;
    }
    // Each step out of the screen brings the HUD an eighth of the way to the eyes.
    sHudW = 1.0f - sTuning[STEREO_HUD] * 0.08f;
}

/*
 * Renderer thread, between frames: applies the anti-aliasing and screen settings, and the 3D slider. 3D is on while
 * the setting is and the slider is up; it turns anti-aliasing and the 800 pixel mode off, which come back with the
 * slider down. A new render target, or a new screen mode, needs the GPU idle, so this waits for it (an empty frame)
 * first; a new target is cleared like the very first one.
 */
static void apply_screen_mode(void) {
    float slider;
    adaptive_restore_main();
    slider = osGet3DSliderState();
    // The switch bench sets DKR_STEREO3D for each race, so the player's 3D setting doesn't change what it measures.
    int stereo = plat_switch("DKR_STEREO3D", sStereoSetting) && slider > 0.0f &&
                 __C3D_Context.gxQueue.maxEntries >= GX_QUEUE_ENTRIES;
    int wide800 = plat_switch("BENCH_WIDE800", s800Setting) && !stereo && !sWideUnsupported;
    int aa = stereo ? PLAT_AA_OFF : sWantedAa;
    int scale = aa != PLAT_AA_OFF ? 2 : 1;
    int across = (aa == PLAT_AA_4X || wide800) ? 2 : 1;
    int newTarget;

    sWideStyleFrame = wide_setting();
    if ((sWideStyleFrame != 0) != sWide && sTarget != NULL) {
        // The game only clears its 4:3 part of the screen: clear the sides of the last wide picture.
        sFrameStarted = 0;
    }
    sWide = sWideStyleFrame != 0;
    sScale3D = sWide ? 1.0f : NARROW;
    apply_stereo_tuning(stereo, slider);
    if (sTarget == NULL ||
        (stereo == sStereo && wide800 == s800 && scale == sRenderScale && across == sScaleX)) {
        return;
    }
    while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        plat_3ds_quit_point(PLAT_3DS_PARKED_GPU_BUSY);
        plat_sleep_ns(250000);
    }
    C3D_FrameEnd(0);
    adaptive_delete_low();
    newTarget = scale != sRenderScale || across != sScaleX;
    if (newTarget) {
        C3D_RenderTargetDelete(sTarget); // Waits for the empty frame.
        if (sTargetRight != NULL) {
            C3D_RenderTargetDelete(sTargetRight); // Made again below, in the new formats.
            sTargetRight = NULL;
        }
        create_target(scale, across);
        sFrameStarted = 0;
        if (wide800 && sScaleX != 2) {
            sWideUnsupported = 1; // No VRAM for the wide target: don't try again every frame.
            wide800 = 0;
        }
    }
    if (stereo && sTargetRight == NULL) {
        // The right eye's target, the same size as the left one's (3D has anti-aliasing off).
        sTargetRight = new_target(TOP_HEIGHT, TOP_WIDTH);
        if (sTargetRight == NULL) {
            plat_log("top screen: no VRAM for the right eye's target, 3D off");
            sStereoSetting = 0;
            stereo = 0;
        } else {
            sFrameStarted = 0; // Cleared like a new target.
        }
    } else if (!stereo && sTargetRight != NULL) {
        C3D_RenderTargetDelete(sTargetRight);
        sTargetRight = NULL;
    }
    if (stereo != sStereo || wide800 != s800) {
        // Takes effect when the next frame is shown. libctru keeps one mode for both: 3D, 800 pixels or neither.
        if (stereo) {
            gfxSet3D(true);
        } else {
            gfxSetWide(wide800);
        }
    }
    sStereo = stereo;
    s800 = wide800;
    link_target();
    plat_log("top screen: %s, render target %dx%d (anti-aliasing %s)", sStereo ? "3D" : s800 ? "800 pixels" : "400 pixels",
             sScaleX, sRenderScale, aa_name(plat_antialias()));
    if (newTarget && scale == 2 && sRenderScale != 2) {
        sWantedAa = PLAT_AA_OFF; // Didn't fit: don't try again every frame.
    }
}

static void alloc_frame_slots(void);

/* An Old 3DS (roadmap O8): the defaults it starts with (until the player picks in Settings) leave out 800 pixels and
 * stereo 3D (which draws everything twice). With the two-core renderer its GPU is mostly idle, and switch bench #266
 * (Jungle Falls) kept 97-98% of frames at 30 with anti-aliasing 2x or Balanced widescreen, so those are its defaults
 * now, as on a New 3DS (whose anti-aliasing default is 4x). The player's choice always wins. */
static int is_old_3ds(void) {
    bool isNew3DS = false;
    APT_CheckNew3DS(&isNew3DS);
    return !isNew3DS;
}

/* The screen settings (see sWideSetting), and whether the console can show 800 pixels across. */
PLAT_COLD static void read_screen_settings(void) {
    u8 model = 0;
    int old3ds = is_old_3ds();
    // An Old 3DS starts with the N64's own 4:3 picture: it is being made to run well at that size first (O8).
    sWideSetting = settings_get("widescreen", 2);
    sWideSetting = sWideSetting < 0 || sWideSetting > 3 ? 2 : sWideSetting;
    s800Setting = settings_get("wide800", !old3ds) != 0;
    sStereoSetting = settings_get("stereo3d", !old3ds) != 0;
    read_stereo_tuning();
    if (R_SUCCEEDED(cfguInit())) {
        if (R_SUCCEEDED(CFGU_GetSystemModel(&model))) {
            sWideUnsupported = model == CFG_MODEL_2DS;
        }
        cfguExit();
    }
    sWide = sWideSetting != 0;
    sScale3D = sWide ? 1.0f : NARROW;
    plat_log("screen: widescreen %d, 800 pixels %d%s, 3D %d", sWideSetting, s800Setting,
             sWideUnsupported ? " (not on a 2DS)" : "", sStereoSetting);
}

void gfxb_init(void) {
    static int sDone;
    u32 white[64];
    int i;

    // Called early from the main thread (plat_init), and again by the renderer thread.
    if (sDone) {
        return;
    }
    sDone = 1;
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2);
    {
        // Anti-aliasing is the Settings menu's choice; until one is made, 4x unless DKR_AA says otherwise (0 off, 1 4x,
        // 2 2x, as settings.txt has it). An fb16 line left in settings.txt by an older build is ignored.
        const char *aa = plat_getenv("DKR_AA");
        sDefaultAa = settings_get("aa", aa != NULL ? atoi(aa) : is_old_3ds() ? PLAT_AA_2X : PLAT_AA_4X);
        if (sDefaultAa < PLAT_AA_OFF || sDefaultAa > PLAT_AA_2X) {
            sDefaultAa = PLAT_AA_4X;
        }
        sWantedAa = sDefaultAa;
        read_screen_settings();
        s800 = s800Setting && !sWideUnsupported;
        create_target(sDefaultAa != PLAT_AA_OFF ? 2 : 1, (sDefaultAa == PLAT_AA_4X || s800) ? 2 : 1);
        if (s800 && sScaleX != 2) {
            sWideUnsupported = 1; // No VRAM for the wide target.
            s800 = 0;
        }
        if (s800) {
            gfxSetWide(true);
        }
        link_target();
        plat_status("Graphics started (anti-aliasing %s%s)", aa_name(plat_antialias()), s800 ? ", 800 pixels" : "");
    }
    sMipmapSetting = settings_get("mipmaps", 1);
    // 2, "Show" (each level tinted), is a debug build's view.
    if (sMipmapSetting < 0 || sMipmapSetting > (PLAT_DEBUG_TOOLS ? 2 : 1)) {
        sMipmapSetting = 1;
    }
    sMipmapsUsed = sMipmapSetting != 0;
    sMipmapsShown = sMipmapSetting == 2;
    sColourSetting = sColourBoost = settings_get("colour", 0);
    if (sColourSetting < 0 || sColourSetting > 3) {
        sColourSetting = sColourBoost = 0;
    }
    plat_log("mipmaps %d, colour boost %d", sMipmapSetting, sColourSetting);

    sShaderDvlb = DVLB_ParseFile((u32 *) shader_shbin, shader_shbin_size);
    shaderProgramInit(&sProgram);
    shaderProgramSetVsh(&sProgram, &sShaderDvlb->DVLE[0]);
    C3D_BindProgram(&sProgram);
    sEyeUniform = shaderInstanceGetUniformLocation(sProgram.vertexShader, "eye");
    init_transform();

    init_vertex_formats();
    alloc_frame_slots();

    for (i = 0; i < 64; i++) {
        white[i] = 0xFFFFFFFF;
    }
    C3D_TexInit(&sWhiteTex, 8, 8, GPU_RGBA8);
    C3D_TexUpload(&sWhiteTex, white);
    gpu_flush(sWhiteTex.data, 8 * 8 * 4);

    C3D_DepthMap(true, -1.0f, 0.0f);
    gpu_self_test();
}

/******************************** Frames ********************************/

/*
 * Frame capture for differential testing: the render target is copied to a
 * linear buffer once the GPU has finished the frame, just before the next one begins, and written out raw: a header
 * of width and height (u32 each) then RGBA8 rows, still in the framebuffer's rotated orientation.
 */
static char sCapturePath[256];

/*
 * Recent frames for bug reports. A glitch that lasts a second is gone by the time the snapshot is taken, so every
 * RECENT_EVERY-th frame the screen's picture is copied, exactly as it was shown, into a ring of RECENT_SLOTS
 * (about a second), both eyes in 3D. The slots are taken at start-up, before textures fill linear memory. A snapshot keeps the two that look most like they hold a bright vertical
 * line (a screen column is a row of the rotated picture). Off unless DKR_RECENT_FRAMES=1: it was for the white
 * vertical line, since fixed (the banana counter icon's sprite overran its buffer).
 */
#define RECENT_SLOTS 8
#define RECENT_EVERY 3 // Odd, so the ring holds both even and odd frames
#define RECENT_KEEP 2
#define RECENT_MAX_BYTES (TOP_HEIGHT * TOP_WIDTH * 2 * 3) // 800 pixels across, 3 bytes each (RGB8, 2 in 16-bit)
static u8 *sRecent[RECENT_SLOTS];
static int sRecentFrame[RECENT_SLOTS], sRecentRows[RECENT_SLOTS], sRecentBpp[RECENT_SLOTS], sRecentEyes[RECENT_SLOTS];
static int sRecentCount, sRecentNext, sRecentFrozen;
static int sRecentOn; // DKR_RECENT_FRAMES (read_switches)

/* Called before C3D_FrameEnd with the screen buffer the frame's display transfer will write (the back buffer until
 * citro3d swaps it in once the GPU is done), and after it to queue the copy. */
static u8 *sRecentScreen;
static int sRecentScreenRows, sRecentScreenBpp; // Bytes per pixel: 3 (BGR8) or 2 (RGB565, with 16-bit buffers)

static u8 *sRecentRight; // The right eye's, in 3D (then 400 rows each, so both fit a slot)

/*
 * Every frame's timing and what the player's kart was doing (plat_get_game_note), so a report shows what happened
 * around the glitch: the CPU time between frame ends, how long the GPU took, and when the top screen's pictures were
 * actually handed to the display. Two pictures handed over in one 16.7 ms screen refresh replace each other (the
 * first is never shown), which is what "replaced" counts.
 */
#define TIMING_FRAMES 64
static struct {
    int frame;
    unsigned int gapUs, gpuUs;
    unsigned int swaps, replaced, swapGapUs; // Since the previous frame end; swapGapUs is between the last two swaps.
    int step;                                // The frame's logic step, in retraces (the game time it covers).
    int haveNote;
    PlatGameNote note;
} sTiming[TIMING_FRAMES];
static int sTimingNext;
static u64 sLastEndTick;

/* Top screen swaps, counted as they happen. citro3d swaps once the GPU has finished a frame (from the GSP event
 * thread), so the swap is wrapped (-Wl,--wrap=gfxScreenSwapBuffers) to see when, not when the CPU ended the frame. */
static volatile unsigned int sSwapCount, sSwapReplaced, sSwapGapUs;
static u64 sLastSwapTick;
/*
 * A third top screen buffer (DKR_TRIPLE_BUFFER, roadmap P35, on by default since switch bench #246: 48 to 60 fps
 * on Crescent Island and Haunted Woods with the same GPU time; off on an Old 3DS, where #251 had it at most 1 fps
 * ahead but its memory left textures without a GPU copy, #252). libctru has two, so a frame finished before the last one reached the screen waits for it
 * (wait_for_present). With three, of our own in linear memory, it is copied into one that is neither on screen nor
 * waiting to be, and carries on. citro3d copies a frame into what gfxGetFramebuffer returns and shows it with
 * gfxScreenSwapBuffers, both wrapped (-Wl,--wrap=...) to use ours: the first while sTripleGet is set (only around
 * the frame's C3D_FrameEnd, so the error console still gets libctru's), the second for a frame handed over with
 * sTripleFrame set (triple_choose). The swap runs on the GSP thread after the GPU finishes, which libctru may report
 * before it has run, so triple_choose first waits for the last one (sTripleSwaps catching up with sTripleQueued)
 * before it changes anything the swap reads.
 * sTripleShown is the buffer on screen, sTripleMaybe one that may have reached it (presented while another was still
 * waiting, just as the screen took that), sTriplePresented the last presented: none of them is written next. They are
 * kept while frames go to libctru's buffers, as one of ours may still be on screen then.
 */
#define TRIPLE_BYTES (GSP_SCREEN_WIDTH * GSP_SCREEN_HEIGHT_TOP_2X * 3) // 800 pixels across or two eyes, 24-bit
#define TRIPLE_TEXTURE_RESERVE (8 * 1024 * 1024) // Linear memory that must stay free for textures (Old 3DS)
static int sTripleWanted;
static volatile int sTripleFrame, sTripleGet;
static volatile unsigned int sTripleQueued, sTripleSwaps;
static u8 *sTriple[3];
static int sTripleNext, sTripleShown = -1, sTripleMaybe = -1, sTriplePresented = -1, sTripleFail;
static unsigned int sTripleId;

u8 *__real_gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16 *width, u16 *height);
__attribute__((used)) u8 *__wrap_gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16 *width, u16 *height) {
    u8 *fb = __real_gfxGetFramebuffer(screen, side, width, height); // libctru's sizes for the mode
    if (screen == GFX_TOP && sTripleGet) {
        fb = sTriple[sTripleNext] + (gfxIs3D() && side != GFX_LEFT ? TRIPLE_BYTES / 2 : 0);
    }
    return fb;
}

/* What libctru's gfxPresentFramebuffer does for the top screen, with our buffer (in linear memory). */
static void triple_present(bool hasStereo) {
    GSPGPU_FramebufferFormat format = gfxGetScreenFormat(GFX_TOP);
    u32 mode = format | (1 << 8);
    const u8 *fbA = sTriple[sTripleNext], *fbB = fbA;
    if (gfxIs3D()) {
        mode |= BIT(5);
        fbB = hasStereo ? fbA + TRIPLE_BYTES / 2 : fbA;
    } else if (!gfxIsWide()) {
        mode |= BIT(6);
    }
    if (gspIsPresentPending(GSP_SCREEN_TOP)) {
        sTripleMaybe = sTriplePresented; // Replaced, unless the screen took it just now.
    } else {
        sTripleShown = sTriplePresented;
        sTripleMaybe = -1;
    }
    sTriplePresented = sTripleNext;
    sTripleId ^= 1;
    gspPresentBuffer(GSP_SCREEN_TOP, sTripleId, fbA, fbB, GSP_SCREEN_WIDTH * gspGetBytesPerPixel(format), mode);
    sTripleSwaps++;
}

void __real_gfxScreenSwapBuffers(gfxScreen_t scr, bool hasStereo);
__attribute__((used)) void __wrap_gfxScreenSwapBuffers(gfxScreen_t scr, bool hasStereo) {
    if (scr == GFX_TOP) {
        u64 now = svcGetSystemTick();
        // Still pending: the previous picture has not reached the screen yet, and this one replaces it.
        if (gspIsPresentPending(GSP_SCREEN_TOP)) {
            sSwapReplaced++;
        }
        sSwapGapUs = sLastSwapTick != 0 ? (unsigned int) ((now - sLastSwapTick) / (SYSCLOCK_ARM11 / 1000000)) : 0;
        sLastSwapTick = now;
        sSwapCount++;
        if (sTripleFrame) {
            triple_present(hasStereo);
            return;
        }
    }
    __real_gfxScreenSwapBuffers(scr, hasStereo);
}

/*
 * Renderer thread, with the GPU done with the last frame and before C3D_FrameEnd: whether this frame goes to our three
 * buffers, and which. Returns 0 when it must wait for the screen as with two (wait_for_present): the setting is off,
 * or the three buffers are all in use (two frames presented within one refresh).
 */
/* Polls (at most ~35 ms) until done() says so; returns whether it did. */
static int triple_poll(int (*done)(void)) {
    int i;
    for (i = 0; i < 70 && !done(); i++) {
        svcSleepThread(500 * 1000);
    }
    return done();
}

static int triple_swapped(void) {
    return sTripleSwaps == sTripleQueued;
}

static int triple_shown(void) {
    return !gspIsPresentPending(GSP_SCREEN_TOP);
}

/* Picks a buffer that is neither on screen, nor may be, nor waiting to be; -1 if there is none. */
static int triple_free(void) {
    int i;
    if (sTriplePresented >= 0 && !gspIsPresentPending(GSP_SCREEN_TOP)) {
        sTripleShown = sTriplePresented;
        sTripleMaybe = -1;
    }
    for (i = 0; i < 3; i++) {
        if (i != sTripleShown && i != sTripleMaybe && i != sTriplePresented) {
            return i;
        }
    }
    return -1;
}

static int triple_choose(void) {
    int i;
    if (sTripleFrame && !triple_poll(triple_swapped)) {
        plat_log("top screen: the last frame's swap never came, two buffers from now");
        sTripleFail = 1;
    }
    sTripleFrame = 0;
    if (!sTripleWanted || sStereo || sTripleFail) {
        return 0;
    }
    if (sTriple[0] == NULL) {
        // Textures come first: an Old 3DS has about 5 MB of linear memory for them, and taking 1.7 MB of it here left
        // 35 textures without a GPU copy (bug report #252).
        if (plat_old_3ds() && linearSpaceFree() < 3 * TRIPLE_BYTES + TRIPLE_TEXTURE_RESERVE) {
            plat_log("top screen: %lu KB of linear memory free, too little for a third buffer: two kept",
                     (unsigned long) (linearSpaceFree() / 1024));
            sTripleFail = 1;
            return 0;
        }
        for (i = 0; i < 3; i++) {
            sTriple[i] = linearAlloc(TRIPLE_BYTES);
            if (sTriple[i] == NULL) {
                plat_log("top screen: no memory for a third buffer, two kept");
                sTripleFail = 1;
                for (i = 0; i < 3; i++) {
                    linearFree(sTriple[i]);
                    sTriple[i] = NULL;
                }
                return 0;
            }
        }
    }
    i = triple_free();
    if (i < 0 && triple_poll(triple_shown)) {
        i = triple_free(); // All three were in use (two frames presented within one refresh): one is free now.
    }
    if (i < 0) {
        return 0; // Still none: wait like two (libctru's back buffer isn't on screen).
    }
    sTripleNext = i;
    sTripleQueued++;
    sTripleFrame = 1;
    return 1;
}

/*
 * citro3d picks the screen buffer a frame's picture is copied into when the frame ends: the one libctru's last swap
 * made the back buffer. But a swap only reaches the screen at the next VBlank, so a frame ending before that (one
 * finished quickly after a slow one, as when a banana or balloon is picked up, or "LAP 2" appears) would have its
 * picture copied into the buffer still on screen, while the screen reads it: a tear down the screen, the white line.
 * So a frame waits, only then, for the last swap to be shown (the GPU carries on drawing it meanwhile).
 * DKR_PRESENT_WAIT=0 = old.
 */
static int sPresentWait = -1;
static unsigned int sPresentWaits;

static void wait_for_present(void) {
    unsigned long long start;
    int i;
    if (sPresentWait < 0) {
        const char *env = plat_getenv("DKR_PRESENT_WAIT");
        sPresentWait = env == NULL || atoi(env) != 0;
    }
    if (!sPresentWait || !gspIsPresentPending(GSP_SCREEN_TOP)) {
        return;
    }
    sPresentWaits++;
    start = plat_time_ns();
    // Polled, at most two refreshes: the next VBlank shows it.
    for (i = 0; i < 70 && gspIsPresentPending(GSP_SCREEN_TOP); i++) {
        svcSleepThread(500 * 1000);
    }
    if (prof_enabled()) {
        // Waiting, like waiting for the GPU, not renderer work.
        unsigned long long ns = plat_time_ns() - start;
        prof_add(PROF_GPU_WAIT, ns);
        prof_part(PROF_PART_PRESENT_WAIT, ns);
    }
}

/*
 * Frame pacing (Old 3DS, roadmap O8). Each frame moves the game on by its logic step: the retraces its main loop
 * measured for the frame before. On an Old 3DS with the renderer on the game's core, the renderer and the game take
 * turns, so a frame is shown whenever its drawing happens to end: steps of 3, 3, 3 could reach the screen 2, 4 and 3
 * retraces apart, and the race jumps although the frame rate looks fine. So a frame that is ready early waits until
 * its step has passed since the last one was shown; one that is late goes at once and the count starts again from
 * it. The wait never makes the gap longer than the last one: the game measures its steps from how long its own
 * frames took, and a frame held back holds the game back too (it waits for a free frame slot), so waiting for whole
 * steps fed on itself and pulled the frame rate down (switch bench #247: 15.0 fps, 17.9 without pacing).
 * DKR_FRAME_PACE=0 = old; on by default on an Old 3DS only, and there only while the frames are handed over from the
 * game's core: on the system core (Renderer System 80%, or the two-core renderer's second half there) its short sleeps
 * wait for the core's next time slice, and bench #261 (Crescent Island, System 80%) ran 15.0 fps with it and 21.1
 * without.
 */
static u32 sPaceShown;    // The retrace the last frame was handed over for.
static int sPaceInterval; // Retraces between the last two hand-overs.
static unsigned int sPaceFrames, sPaceWaits, sPaceLate;

/* Every 300 frames: how many waited to be on time, and how many were late (shown later than their step). */
static void pace_count(int waited, int late) {
    sPaceFrames++;
    sPaceWaits += waited;
    sPaceLate += late;
    if (sPaceFrames == 300) {
        plat_log("frame pacing: of %u frames, %u waited to be on time, %u were late", sPaceFrames, sPaceWaits,
                 sPaceLate);
        sPaceFrames = sPaceWaits = sPaceLate = 0;
    }
}

static void pace_frame(void) {
    u32 now = gPlatCounters[PLAT_COUNTER_VI];
    int step = plat_frame_step();
    s32 ahead;
    if (!plat_switch("DKR_FRAME_PACE", plat_old_3ds() && svcGetProcessorID() == 0) || step < 1 || step > 6) {
        sPaceInterval = (int) (now - sPaceShown);
        sPaceShown = now;
        return;
    }
    if (step > sPaceInterval) {
        step = sPaceInterval;
    }
    // A frame handed over now reaches the screen at the next retrace (the GPU takes a few ms): the last one handed
    // over at retrace P was shown at P + 1, so this one, due step retraces after it, is handed over at P + step.
    ahead = (s32) (sPaceShown + step - now);
    if (ahead <= 0 || ahead > 6) {
        sPaceInterval = (int) (now - sPaceShown);
        sPaceShown = now; // Late (or after a load or the HOME menu): shown at once, the next counts from it.
        pace_count(0, ahead < 0);
        return;
    }
    pace_count(1, 0);
    {
        unsigned long long start = plat_time_ns();
        while ((s32) (gPlatCounters[PLAT_COUNTER_VI] - (now + ahead)) < 0) {
            svcSleepThread(1000 * 1000);
        }
        if (prof_enabled()) {
            unsigned long long ns = plat_time_ns() - start;
            prof_add(PROF_GPU_WAIT, ns);
            prof_part(PROF_PART_PRESENT_WAIT, ns);
        }
    }
    sPaceInterval = (int) (now + ahead - sPaceShown);
    sPaceShown = now + ahead;
}

static u8 *sScreenSeen[2], *sShownScreen;

static void find_recent_screen(void) {
    u16 w = 0, h = 0;
    GSPGPU_FramebufferFormat format = gfxGetScreenFormat(GFX_TOP);
    sRecentScreen = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
    sRecentScreenBpp = format == GSP_BGR8_OES ? 3 : format == GSP_RGB565_OES ? 2 : 0;
    sRecentScreenRows = (w == TOP_HEIGHT && sRecentScreenBpp != 0) ? h : 0;
    sRecentRight = sStereo && h == TOP_WIDTH ? gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL) : NULL;
    // The other of the two screen buffers is the one on screen (once both have been seen).
    if (sRecentScreen != sScreenSeen[0] && sRecentScreen != sScreenSeen[1]) {
        sScreenSeen[1] = sScreenSeen[0];
        sScreenSeen[0] = sRecentScreen;
    }
    sShownScreen = sRecentScreen == sScreenSeen[0] ? sScreenSeen[1] : sScreenSeen[0];
}

static void record_timing(void) {
    static unsigned int lastSwaps, lastReplaced;
    static int cleared;
    u64 now = svcGetSystemTick();
    unsigned int swaps = sSwapCount, replaced = sSwapReplaced;
    int i;
    if (!cleared) {
        for (i = 0; i < TIMING_FRAMES; i++) {
            sTiming[i].frame = -1; // Empty (frame 0 is a real frame).
        }
        cleared = 1;
    }
    if (!sRecentFrozen) {
        sTiming[sTimingNext].frame = sFrameCount - 1;
        sTiming[sTimingNext].gapUs = sLastEndTick != 0 ? (unsigned int) ((now - sLastEndTick) / (SYSCLOCK_ARM11 / 1000000)) : 0;
        sTiming[sTimingNext].gpuUs = (unsigned int) (C3D_GetDrawingTime() * 1000.0f);
        sTiming[sTimingNext].swaps = swaps - lastSwaps;
        sTiming[sTimingNext].replaced = replaced - lastReplaced;
        sTiming[sTimingNext].swapGapUs = sSwapGapUs;
        sTiming[sTimingNext].step = plat_frame_step();
        sTiming[sTimingNext].haveNote = plat_get_game_note(&sTiming[sTimingNext].note);
        sTimingNext = (sTimingNext + 1) % TIMING_FRAMES;
    }
    lastSwaps = swaps;
    lastReplaced = replaced;
    sLastEndTick = now;
}

PLAT_COLD static void log_timing(void) {
    int i;
    plat_log("last %d frames (CPU time since the previous frame ended, GPU time of the one before, top screen swaps "
             "since then and the time between the last two, logic step, player):", TIMING_FRAMES);
    for (i = 0; i < TIMING_FRAMES; i++) {
        int k = (sTimingNext + i) % TIMING_FRAMES;
        char note[80];
        if (sTiming[k].frame < 0) {
            continue;
        }
        if (sTiming[k].haveNote) {
            plat_format_game_note(&sTiming[k].note, note, sizeof(note));
        } else {
            plat_snprintf(note, sizeof(note), "(player note busy)");
        }
        plat_log("  frame %d: +%u.%u ms, gpu %u.%u ms, %u swap%s +%u.%u ms%s, step %d, %s", sTiming[k].frame,
                 sTiming[k].gapUs / 1000, sTiming[k].gapUs / 100 % 10, sTiming[k].gpuUs / 1000,
                 sTiming[k].gpuUs / 100 % 10, sTiming[k].swaps, sTiming[k].swaps == 1 ? "" : "s",
                 sTiming[k].swapGapUs / 1000, sTiming[k].swapGapUs / 100 % 10,
                 sTiming[k].replaced != 0 ? " (REPLACED one before it was shown)" : "", sTiming[k].step, note);
    }
}

/* Takes the ring's slots, as many as fit; called at start-up, or when the switch bench turns the ring on. Slots once
 * taken are kept. */
PLAT_COLD static void allocate_recent_frames(void) {
    // Leave most of linear memory for textures (each slot is 576 KB).
    while (sRecentCount < RECENT_SLOTS && linearSpaceFree() >= RECENT_MAX_BYTES + 14 * 1024 * 1024) {
        sRecent[sRecentCount] = linearAlloc(RECENT_MAX_BYTES);
        if (sRecent[sRecentCount] == NULL) {
            break;
        }
        sRecentFrame[sRecentCount++] = -1;
    }
    plat_log("recent frames for bug reports: %d slots, one frame in %d (%lu KB of linear memory left)", sRecentCount,
             RECENT_EVERY, (unsigned long) (linearSpaceFree() / 1024));
}

/*
 * The line watch: every frame, the last finished picture (the back buffer, before this frame's transfer is queued
 * into it) is checked for a bright vertical line, sampling every 8th pixel down each screen column. The first one
 * found sends a bug report by itself, so its snapshot (the next frame's draws, while the line is still there) shows
 * what drew it. Every frame, so a line shown only on odd or only on even frames is caught too; races score up to
 * about 12 of 30. Off unless DKR_LINE_WATCH=1 (or 2), as the line is fixed.
 */
#define LINE_WATCH_SAMPLES (TOP_HEIGHT / 8)
#define LINE_WATCH_FOUND (LINE_WATCH_SAMPLES / 2)
void bugreport_request(void);
static int sLineWatch, sLineReported, sLineLogged, sLineBest, sLineBestColumn;

/* How much a picture looks like it holds a bright vertical line: the most pixels in one screen column (a row here)
 * that are clearly brighter than the columns 3 to each side, checking every step-th pixel down it. */
static inline int pixel_luma(const u8 *p, int bpp) {
    if (bpp == 2) {
        int c = p[0] | (p[1] << 8); // RGB565
        return ((c >> 11) << 3) + (((c >> 5) & 0x3F) << 3) + ((c & 0x1F) << 3); // 0..1020
    }
    return p[0] + p[1] * 2 + p[2]; // 0..1020
}

/* In 3D (eyes 2) each eye is scored on its own, so rows at the edge of one eye are never compared with the other's. */
static int line_score(const u8 *pixels, int rows, int bpp, int first, int step, int eyes, int *where) {
    int r, x, best = 0, stride = TOP_HEIGHT * bpp, eyeRows = rows / eyes;
    *where = -1;
    for (r = 3; r < rows - 3; r++) {
        if (r % eyeRows < 3 || r % eyeRows >= eyeRows - 3) {
            continue;
        }
        const u8 *row = pixels + r * stride, *left = row - 3 * stride, *right = row + 3 * stride;
        int count = 0;
        for (x = first; x < TOP_HEIGHT; x += step) {
            int c = pixel_luma(row + x * bpp, bpp);
            if (c > pixel_luma(left + x * bpp, bpp) + 80 && c > pixel_luma(right + x * bpp, bpp) + 80) {
                count++;
            }
        }
        if (count > best) {
            best = count;
            *where = r;
        }
    }
    return best;
}

/* Logs where a line was found and the bytes under it, to tell a stray memory write (samples, a game structure) from
 * something drawn. */
PLAT_COLD static void log_line_bytes(const u8 *buffer, int row) {
    const u8 *p = buffer + row * TOP_HEIGHT * sRecentScreenBpp;
    char hex[3 * 48 + 1];
    int i;
    for (i = 0; i < 48; i++) {
        snprintf(hex + i * 3, 4, "%02x ", p[i]);
    }
    plat_log("line watch: at %p (screen buffer %p + %d): %s", (const void *) p, (const void *) buffer,
             row * TOP_HEIGHT * sRecentScreenBpp, hex);
}

static void watch_for_line(void) {
    // The finished picture about to be replaced (each eye), and the one on screen: something written into that one
    // after its picture was copied in is only ever seen there.
    u8 *eyes[3] = { sRecentScreen, sRecentRight, sShownScreen };
    static const char *const names[3] = { "left eye", "right eye", "shown picture" };
    int e, where, score;
    if (!sLineWatch || sRecentScreenRows == 0 || sRecentFrozen) {
        return;
    }
    if ((u32) sRecentScreen >= OS_VRAM_VADDR && (u32) sRecentScreen < OS_VRAM_VADDR + OS_VRAM_SIZE &&
        sLineWatch != 2) {
        return; // Reading VRAM from the CPU is too slow to do every frame.
    }
    for (e = 0; e < 3; e++) {
        if (eyes[e] == NULL) {
            continue;
        }
        GSPGPU_InvalidateDataCache(eyes[e], sRecentScreenRows * TOP_HEIGHT * sRecentScreenBpp);
        if (sLineWatch == 2 && sFrameCount >= 1800 && sFrameCount < 1830) {
            // DKR_LINE_WATCH=2 tests it: a white line down the middle of the old picture a minute in.
            memset(eyes[e] + sRecentScreenRows / 2 * TOP_HEIGHT * sRecentScreenBpp, 255, TOP_HEIGHT * sRecentScreenBpp);
        }
        // Every 8th pixel down the column, starting 4 further on odd frames.
        score = line_score(eyes[e], sRecentScreenRows, sRecentScreenBpp, (sFrameCount & 1) * 4, 8, 1, &where);
        if (score > sLineBest) {
            sLineBest = score;
            sLineBestColumn = where * TOP_WIDTH / sRecentScreenRows;
        }
        if (score >= LINE_WATCH_FOUND && sLineLogged < 20) {
            PlatGameNote gameNote;
            char note[96] = "player unknown";
            if (plat_get_game_note(&gameNote)) {
                plat_format_game_note(&gameNote, note, sizeof(note));
            }
            sLineLogged++;
            plat_log("line watch: frame %d %s has a line at column %d (%d of %d), %s", sFrameCount - 2, names[e],
                     where * TOP_WIDTH / sRecentScreenRows, score, LINE_WATCH_SAMPLES, note);
            log_line_bytes(eyes[e], where);
            if (!sLineReported) {
                sLineReported = 1;
                bugreport_request();
            }
        }
    }
    if (sFrameCount % 150 == 0) {
        plat_log("line watch: best %d of %d at column %d in the last 5 s; %u frames waited for the screen", sLineBest,
                 LINE_WATCH_SAMPLES, sLineBestColumn, sPresentWaits);
        sPresentWaits = 0;
        sLineBest = 0;
    }
}

static void record_recent_frame(void) {
    u32 bytes = sRecentScreenRows * TOP_HEIGHT * sRecentScreenBpp;
    u8 *slot;

    record_timing();
    if (!sRecentOn || sRecentCount <= 0 || sRecentFrozen || sFrameCount % RECENT_EVERY != 0 || sRecentScreen == NULL ||
        bytes == 0 || bytes > RECENT_MAX_BYTES) {
        return;
    }
    slot = sRecent[sRecentNext];
    // A plain copy of the screen buffer, queued after the frame's display transfer, so it holds what was shown. The
    // buffer is only drawn into again two frames later, and the next frame waits for this queue to finish.
    GX_TextureCopy((u32 *) sRecentScreen, 0, (u32 *) slot, 0, bytes, GX_TRANSFER_RAW_COPY(1));
    sRecentFrame[sRecentNext] = sFrameCount - 1; // The frame just ended (sFrameCount already counts it).
    sRecentRows[sRecentNext] = sRecentScreenRows;
    sRecentBpp[sRecentNext] = sRecentScreenBpp;
    sRecentEyes[sRecentNext] = 1;
    if (sRecentRight != NULL && bytes * 2 <= RECENT_MAX_BYTES) {
        // In 3D the right eye goes after the left, so the saved picture shows the two side by side.
        GX_TextureCopy((u32 *) sRecentRight, 0, (u32 *) (slot + bytes), 0, bytes, GX_TRANSFER_RAW_COPY(1));
        sRecentRows[sRecentNext] *= 2;
        sRecentEyes[sRecentNext] = 2;
    }
    sRecentNext = (sRecentNext + 1) % sRecentCount;
}

/* Saves the recent frames most likely to show a line as diff_NNNNN_recentK.bin (width, height, then BGR8), and
 * logs every frame's score. Called once the GPU has finished the snapshot frame, so the ring is idle. */
PLAT_COLD static void save_recent_frames(int snapshotFrame) {
    int score[RECENT_SLOTS], where[RECENT_SLOTS], i, k;
    char path[256];

    log_timing();
    for (i = 0; i < sRecentCount; i++) {
        score[i] = -1;
        if (sRecentFrame[i] >= 0) {
            int eyeRows = sRecentRows[i] / sRecentEyes[i];
            GSPGPU_InvalidateDataCache(sRecent[i], sRecentRows[i] * TOP_HEIGHT * sRecentBpp[i]);
            score[i] = line_score(sRecent[i], sRecentRows[i], sRecentBpp[i], 0, 1, sRecentEyes[i], &where[i]);
            // Columns are counted in the 400-pixel screen, whatever the render width.
            plat_log("recent frame %d: line score %d at %scolumn %d", sRecentFrame[i], score[i],
                     sRecentEyes[i] == 1 || where[i] < 0 ? "" : (where[i] < eyeRows ? "left eye, " : "right eye, "),
                     where[i] < 0 ? -1 : (where[i] % eyeRows) * TOP_WIDTH / eyeRows);
        }
    }
    for (k = 0; k < RECENT_KEEP; k++) {
        int pick = -1;
        for (i = 0; i < sRecentCount; i++) {
            if (score[i] >= 0 && (pick < 0 || score[i] > score[pick])) {
                pick = i;
            }
        }
        if (pick >= 0) {
            u32 header[2] = { TOP_HEIGHT, (u32) sRecentRows[pick] };
            u32 pixels = sRecentRows[pick] * TOP_HEIGHT, bytes = pixels * 3, p;
            u8 *file = malloc(8 + bytes);
            score[pick] = -1;
            if (file != NULL) {
                memcpy(file, header, 8);
                if (sRecentBpp[pick] == 3) {
                    memcpy(file + 8, sRecent[pick], bytes);
                } else {
                    // 16-bit buffers: saved as BGR8 all the same.
                    for (p = 0; p < pixels; p++) {
                        int c = sRecent[pick][p * 2] | (sRecent[pick][p * 2 + 1] << 8);
                        file[8 + p * 3] = (c & 0x1F) << 3;
                        file[8 + p * 3 + 1] = ((c >> 5) & 0x3F) << 2;
                        file[8 + p * 3 + 2] = (c >> 11) << 3;
                    }
                }
                plat_snprintf(path, sizeof(path), "%s/diff_%05d_recent%d.bin", plat_data_dir(), snapshotFrame, k + 1);
                plat_save_file(path, file, 8 + bytes);
                free(file);
                plat_log("kept recent frame %d as recent%d", sRecentFrame[pick], k + 1);
            }
        }
    }
}

void gfxb_capture_frame(const char *path) {
    snprintf(sCapturePath, sizeof(sCapturePath), "%s", path);
}

static void capture_pending_frame(void) {
    // The framebuffer is rotated (240 wide, 400 tall, times the render scale). It is copied at full resolution; the
    // comparison tool downsamples anti-aliased captures itself. 2x anti-aliasing (doubled one way only) is halved
    // here, which the tool doesn't expect.
    const int halve = sRenderScale == 2 && sScaleX == 1;
    const u32 w = TOP_HEIGHT * (halve ? 1 : sRenderScale), h = TOP_WIDTH * sScaleX;
    u32 *pixels;
    FILE *file;

    if (sCapturePath[0] == '\0') {
        return;
    }
    pixels = linearAlloc(w * h * 4);
    if (pixels != NULL) {
        gpu_flush(pixels, w * h * 4);
        C3D_SyncDisplayTransfer((u32 *) sTarget->frameBuf.colorBuf,
                                GX_BUFFER_DIM(TOP_HEIGHT * sRenderScale, TOP_WIDTH * sScaleX), pixels,
                                GX_BUFFER_DIM(w, h),
                                GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                    GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                    GX_TRANSFER_SCALING(halve ? GX_TRANSFER_SCALE_X : GX_TRANSFER_SCALE_NO));
        GSPGPU_InvalidateDataCache(pixels, w * h * 4);
        file = fopen(sCapturePath, "wb");
        if (file != NULL) {
            fwrite(&w, 4, 1, file);
            fwrite(&h, 4, 1, file);
            fwrite(pixels, 4, w * h, file);
            fclose(file);
        }
        linearFree(pixels);
    }
    sCapturePath[0] = '\0';
}

static void draw_clear_rect(const short rect[4], u32 rgba, int depthOnly);

static int sLogFrame = -2; // Frame whose draws are logged (DKR_LOG_DRAWS, or a snapshot)
static int sSnapshotFrame = -1;

/*
 * Snapshots also check that the textures the frame used still hold the same data once the GPU has drawn it (they are
 * checked when drawn too), and save the frame's GPU command list, to see exactly what the GPU was told.
 */
#define MAX_CHECKED_TEXTURES 512
static struct {
    const C3DTexture *tex;
    const void *data;
    u32 *copy; // What the memory held when the texture was drawn
} sChecked[MAX_CHECKED_TEXTURES];
static int sNumChecked;
static int sSnapshotPending;

static u32 texture_words(const C3DTexture *t) {
    return t->tex.size / 4;
}

/* Makes the CPU see the memory the GPU reads (flushing first, so nothing the CPU wrote is thrown away). */
static void sync_from_memory(const void *data, u32 size) {
    GSPGPU_FlushDataCache(data, size);
    GSPGPU_InvalidateDataCache(data, size);
}

PLAT_COLD static void remember_texture(const C3DTexture *t) {
    u32 size = texture_words(t) * 4;
    u32 *copy;
    int i;
    for (i = 0; i < sNumChecked; i++) {
        if (sChecked[i].tex == t) {
            return;
        }
    }
    if (sNumChecked == MAX_CHECKED_TEXTURES || (copy = malloc(size)) == NULL) {
        return;
    }
    sync_from_memory(t->tex.data, size);
    memcpy(copy, t->tex.data, size);
    sChecked[sNumChecked].tex = t;
    sChecked[sNumChecked].data = t->tex.data;
    sChecked[sNumChecked].copy = copy;
    sNumChecked++;
}

static void save_words(const char *what, int index, const void *data, u32 words) {
    char path[256];
    FILE *f;
    plat_snprintf(path, sizeof(path), "%s/diff_%05d_tex%d_%s.bin", plat_data_dir(), sSnapshotFrame, index, what);
    f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(data, 4, words, f);
        fclose(f);
    }
}

/* Once the GPU has finished the snapshot frame, and before any texture is deleted. */
PLAT_COLD static void check_textures_after_frame(void) {
    int i, j, changed = 0;
    for (i = 0; i < sNumChecked; i++) {
        const C3DTexture *t = sChecked[i].tex;
        const u32 *now = (const u32 *) sChecked[i].data;
        u32 n = texture_words(t), k, wrong = 0, first = 0;
        u32 values[4] = { 0 }, counts[4] = { 0 };
        sync_from_memory(now, n * 4);
        for (k = 0; k < n; k++) {
            if (now[k] != sChecked[i].copy[k]) {
                if (wrong++ == 0) {
                    first = k;
                }
                // The most common new values say what overwrote it.
                for (j = 0; j < 4; j++) {
                    if (counts[j] == 0 || values[j] == now[k]) {
                        values[j] = now[k];
                        counts[j]++;
                        break;
                    }
                }
            }
        }
        if (wrong > 0 || t->tex.data != sChecked[i].data) {
            plat_log("after the GPU: texture %d at %p (%dx%d, now at %p): %lu of %lu words changed from word %lu; new "
                     "values %08lX x%lu, %08lX x%lu, %08lX x%lu, %08lX x%lu",
                     i, sChecked[i].data, t->tex.width, t->tex.height, t->tex.data, (unsigned long) wrong,
                     (unsigned long) n, (unsigned long) first, (unsigned long) values[0], (unsigned long) counts[0],
                     (unsigned long) values[1], (unsigned long) counts[1], (unsigned long) values[2],
                     (unsigned long) counts[2], (unsigned long) values[3], (unsigned long) counts[3]);
            if (changed < 4) {
                save_words("drawn", i, sChecked[i].copy, n);
                save_words("after", i, now, n);
            }
            changed++;
        }
        free(sChecked[i].copy);
    }
    plat_log("after the GPU: %d of %d textures changed", changed, sNumChecked);
    sNumChecked = 0;
}

/* Where the other buffers in GPU memory are, to see what lies next to a texture that changed. */
PLAT_COLD static void log_buffers(void) {
    u16 w, h;
    u8 *top0, *top1, *bottom;
    top0 = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
    plat_log("buffers: command list %p (%lu bytes), vertices %p (%lu bytes), white texture %p, colour %p, depth %p",
             (void *) gpuCmdBuf, (unsigned long) gpuCmdBufSize * 4, (void *) sVbo,
             (unsigned long) VBO_BYTES, sWhiteTex.data, sTarget->frameBuf.colorBuf,
             sTarget->frameBuf.depthBuf);
    top1 = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, &w, &h);
    bottom = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
    plat_log("buffers: top screen %p / %p (%ux%u), bottom screen %p, linear heap %p (%lu bytes), %lu free, %lu KB "
             "of VRAM free",
             (void *) top0, (void *) top1, w, h, (void *) bottom, (void *) (uintptr_t) __ctru_linear_heap,
             (unsigned long) __ctru_linear_heap_size, (unsigned long) linearSpaceFree(),
             (unsigned long) (vramSpaceFree() / 1024));
}

PLAT_COLD static void save_command_list(void) {
    char path[256];
    FILE *f;
    plat_snprintf(path, sizeof(path), "%s/diff_%05d_cmd.bin", plat_data_dir(), sSnapshotFrame);
    f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(gpuCmdBuf, 4, gpuCmdBufOffset, f);
        fclose(f);
    }
    plat_log("command list: %lu words at %p saved to %s", (unsigned long) gpuCmdBufOffset, (void *) gpuCmdBuf,
             f != NULL ? path : "nowhere (could not open the file)");
}

/*
 * For a GPU hang. Each draw of a frame is noted in a few words, always: if the GPU then stops responding, the frame it
 * is stuck on is still the last one drawn, and gpu_hang.txt lists its draws next to the GPU's command queue, which
 * shows the command list the GPU was working on. gpu_hang_cmd.bin holds the frame's whole command list.
 */
typedef struct HangDraw {
    u32 cmdStart, cmdEnd; // Word offsets of the draw's commands in the frame's command list
    /* The draw's settings (a GfxDrawState) up to its textures: one block copy a draw, instead of picking the fields
     * the report prints out one by one (hang_draw_start is on the per-draw path). */
    u8 state[offsetof(GfxDrawState, textures)];
    u16 triangles;
    const C3D_Tex *tex[2];
} HangDraw;
#define HANG_DRAWS 1024
static int sHangSaved;

/*
 * The CPU and the GPU work on different frames at the same time (roadmap item P3). The renderer used to wait for the
 * GPU to finish the last frame before building the next one, because there was one vertex buffer and one command
 * list, so a frame cost the renderer's time plus the GPU's. Now there are two frame slots: while the GPU draws the
 * frame in one, the renderer builds the next in the other, and only waits for the GPU when it hands that frame over
 * (gfxb_end_frame). Nothing of a frame reaches the GPU before then: the command list splits that let the GPU finish
 * before a blended draw (drain_before_draw) are kept as parts and queued with the rest.
 * Everything a slot holds lives in linear memory (FCRAM), which the GPU reads directly; VRAM only holds the render
 * target. Textures freed while a frame is built are deleted once the GPU has drawn that frame.
 * DKR_OVERLAP=0 waits for the GPU before building each frame, as before (still alternating the slots).
 */
#define MAX_LIST_PARTS 26 // With the rest of the list, the display transfer and a clear, under citro3d's 32 entries
typedef struct FrameSlot {
    u8 *vbo;             // VBO_BYTES of vertices
    u16 *ibo;            // IBO_BYTES of vertex indices
    u32 *cmd;            // The command list, sCmdWords long
    u32 *partAddr[MAX_LIST_PARTS];
    u32 partWords[MAX_LIST_PARTS];
    int numParts;        // Parts split off the list, queued before the rest when the frame is handed over
    C3DTexture **frees;  // Textures freed while the frame was built
    int numFrees, freesCapacity;
    HangDraw draws[HANG_DRAWS];
    int numDraws;        // Draws in the frame, which can be more than HANG_DRAWS
    int frame;           // sFrameCount when it was built
    u32 words;           // Command list words used
    u32 *eyeCmd;         // EYE_CMD_WORDS: the two eyes' short command lists of a stereo frame made once
    void **meshFrees;    // Meshes freed while the frame was built (gfxb_mesh_free)
    int numMeshFrees, meshFreesCapacity;
    int adaptAa;         // Adaptive anti-aliasing: 0 not in use, 1 drawn anti-aliased, 2 at the low resolution
} FrameSlot;
static FrameSlot sSlots[2];
static int sNumSlots = 1; // 2 unless the second slot didn't fit
static int sRec;          // The slot being built
static int sOnGpu = -1;   // The slot last handed to the GPU
static u32 sCmdWords;
static int sOverlap;      // This frame is built while the GPU draws the last one

bool C3Di_SplitFrame(u32 **pBuf, u32 *pSize);

static u32 cmd_position(void) {
    return (u32) (gpuCmdBuf + gpuCmdBufOffset - sSlots[sRec].cmd);
}

/* Two frame slots, each with a vertex buffer and a command list as large as citro3d's own. */
PLAT_COLD static void alloc_frame_slots(void) {
    gxCmdQueue_s *queue = &__C3D_Context.gxQueue;
    gxCmdEntry_s *entries;
    int i;
    sCmdWords = __C3D_Context.cmdBufSize;
    for (i = 0; i < 2; i++) {
        sSlots[i].vbo = linearAlloc(VBO_BYTES);
        sSlots[i].ibo = linearAlloc(IBO_BYTES);
        sSlots[i].cmd = linearAlloc(sCmdWords * 4);
        sSlots[i].eyeCmd = linearAlloc(EYE_CMD_WORDS * 4);
        if (sSlots[i].vbo == NULL || sSlots[i].ibo == NULL || sSlots[i].cmd == NULL) {
            if (sSlots[i].vbo != NULL) {
                linearFree(sSlots[i].vbo);
            }
            if (sSlots[i].ibo != NULL) {
                linearFree(sSlots[i].ibo);
            }
            if (sSlots[i].cmd != NULL) {
                linearFree(sSlots[i].cmd);
            }
            sSlots[i].vbo = NULL;
            sSlots[i].ibo = NULL;
            sSlots[i].cmd = NULL;
            break;
        }
    }
    // The queue is empty and stopped this early. Without the longer one, 3D stays off (apply_screen_mode).
    entries = malloc(GX_QUEUE_ENTRIES * sizeof(gxCmdEntry_s));
    if (entries != NULL) {
        free(queue->entries);
        queue->entries = entries;
        queue->maxEntries = GX_QUEUE_ENTRIES;
    }
    sNumSlots = i > 0 ? i : 1; // None can't happen this early, and nothing could be drawn.
    plat_log("frame slots: %d, each %lu KB of vertices and %lu KB of commands in linear memory, %lu KB left",
             sNumSlots, (unsigned long) (VBO_BYTES / 1024),
             (unsigned long) (sCmdWords * 4 / 1024), (unsigned long) (linearSpaceFree() / 1024));
    sVbo = sSlots[0].vbo;
    sIbo = sSlots[0].ibo;
    if (plat_switch("DKR_RECENT_FRAMES", 0)) {
        allocate_recent_frames();
    }
}

/* Ends the part of the command list so far, which the GPU then finishes before it starts on the rest. */
static void split_command_list(void) {
    FrameSlot *slot = &sSlots[sRec];
    u32 *addr, words;
    if (slot->numParts < MAX_LIST_PARTS && C3Di_SplitFrame(&addr, &words)) {
        slot->partAddr[slot->numParts] = addr;
        slot->partWords[slot->numParts] = words;
        slot->numParts++;
    }
}

/******************************** Stereo ********************************/

/*
 * Stereo 3D (roadmap item G5), the way the SM64 3DS port does it: each eye has its own render target (sTarget for the
 * left, sTargetRight for the right), and every draw is sent twice, once to each, with the shader's eye offset
 * (shader.v.pica) set for that eye in between. citro3d copies both targets to their framebuffers at the end of the
 * frame. Everything is transformed on the CPU, so both draws read the same vertices. The GPU draws everything twice:
 * anti-aliasing and the 800 pixel mode are off while 3D is on. Flat draws (HUD, text, menus) sit at the depth of the
 * screen (to_screen_flat).
 */

/* Writes the shader's eye offset to the command list: the vertex moves across by mul * w + add (in clip space). */
static void write_eye(float mul, float add) {
    C3D_FVec v;
    if (sEyeUniform < 0) {
        return;
    }
    v.x = mul;
    v.y = add;
    v.z = 0.0f;
    v.w = 0.0f;
    GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG, 0x80000000u | (u32) sEyeUniform);
    GPUCMD_AddWrites(GPUREG_VSH_FLOATUNIFORM_DATA, (u32 *) v.c, 4);
}

/*
 * Points the GPU at a render target, written straight to the command list as citro3d's C3D_FrameBufBind does (both
 * targets have the same size and formats). citro3d's C3D_SetFrameBuf only works inside one of its frames, and a frame
 * is mostly built before citro3d's begins (while the GPU draws the last one, see wait_for_gpu), so it can't be used
 * here: it left every draw on the left eye's target.
 */
static void bind_target(C3D_RenderTarget *target) {
    const C3D_FrameBuf *fb = &target->frameBuf;
    u32 param[3];
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    param[0] = osConvertVirtToPhys(fb->depthBuf) >> 3;
    param[1] = osConvertVirtToPhys(fb->colorBuf) >> 3;
    param[2] = 0x01000000 | (((u32) (fb->height - 1) & 0xFFF) << 12) | (fb->width & 0xFFF);
    GPUCMD_AddIncrementalWrites(GPUREG_DEPTHBUFFER_LOC, param, 3);
    GPUCMD_AddWrite(GPUREG_RENDERBUF_DIM, param[2]);
}

/* Draws count vertices from vertex on, or the count vertices the indices name, once for each eye (3D). Out of line
 * so that the per-draw path of a frame drawn once (draw_prims) stays short. */
static void __attribute__((noinline)) draw_prims_stereo(int vertex, int count, const u16 *indices) {
    // sEyeSep is positive for the left eye's offset; what is sEyeConv away stays at the depth of the screen.
    bind_target(sTarget);
    write_eye(sEyeSep, -sEyeSep * sEyeConv);
    if (indices != NULL) {
        C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, indices);
    } else {
        C3D_DrawArrays(GPU_TRIANGLES, vertex, count);
    }
    bind_target(sTargetRight);
    write_eye(-sEyeSep, sEyeSep * sEyeConv);
    if (indices != NULL) {
        C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, indices);
    } else {
        C3D_DrawArrays(GPU_TRIANGLES, vertex, count);
    }
}

/* Draws count vertices from vertex on, or the count vertices the indices name; once, or in 3D once for each eye. */
static inline void draw_prims(int vertex, int count, const u16 *indices) {
    if (sStereo && !sOnceFrame) {
        draw_prims_stereo(vertex, count, indices);
    } else if (indices != NULL) {
        C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, indices);
    } else {
        C3D_DrawArrays(GPU_TRIANGLES, vertex, count);
    }
}

/*
 * Stereo frames made once (roadmap item P33, DKR_STEREO_ONCE=1): instead of every draw being sent twice, the frame's
 * commands are made once and the GPU runs them twice, the way the F-Zero X 3DS port does it. Each eye gets a short
 * command list of its own that sets its target and eye offset, and the GPU queue gets the left eye's, the frame's,
 * the right eye's and the frame's again. That saves the CPU writing every draw a second time, and the GPU a
 * framebuffer flush and a change of target between every two draws. The frame's commands make no assumption about
 * the settings they start with (see gfxb_start_frame), and command list splits (DKR_GPU_SPLIT) leave it off.
 */
static void stereo_once_lists(FrameSlot *slot) {
    u32 *body, bodyWords, *eye[2], eyeWords[2], *saved = gpuCmdBuf, savedSize = gpuCmdBufSize;
    int i;
    if (slot->eyeCmd == NULL || slot->numParts + 4 > MAX_LIST_PARTS || !C3Di_SplitFrame(&body, &bodyWords)) {
        return;
    }
    saved = gpuCmdBuf; // After the frame's commands: the frame's end goes on from there.
    savedSize = gpuCmdBufSize;
    GPUCMD_SetBuffer(slot->eyeCmd, EYE_CMD_WORDS, 0);
    for (i = 0; i < 2; i++) {
        bind_target(i == 0 ? sTarget : sTargetRight);
        write_eye(i == 0 ? sEyeSep : -sEyeSep, i == 0 ? -sEyeSep * sEyeConv : sEyeSep * sEyeConv);
        GPUCMD_Split(&eye[i], &eyeWords[i]);
    }
    GPUCMD_SetBuffer(saved, savedSize, 0);
    for (i = 0; i < 2; i++) {
        slot->partAddr[slot->numParts] = eye[i];
        slot->partWords[slot->numParts++] = eyeWords[i];
        slot->partAddr[slot->numParts] = body;
        slot->partWords[slot->numParts++] = bodyWords;
    }
}

PLAT_HOT static HangDraw *hang_draw_start(const GfxDrawState *state, int numTriangles) {
    FrameSlot *slot = &sSlots[sRec];
    HangDraw *d;
    int i;
    if (slot->numDraws >= HANG_DRAWS) {
        slot->numDraws++;
        return NULL;
    }
    d = &slot->draws[slot->numDraws++];
    d->cmdStart = cmd_position();
    d->cmdEnd = d->cmdStart;
    d->triangles = numTriangles;
    memcpy(d->state, state, sizeof(d->state));
    for (i = 0; i < 2; i++) {
        // The C3D_Tex is the first member of a C3DTexture, which outlives the frame (process_pending_frees).
        d->tex[i] = (state->textures[i] != NULL) ? (const C3D_Tex *) state->textures[i]->backendData : NULL;
    }
    return d;
}

/* Writes gpu_hang.txt and gpu_hang_cmd.bin, once, when the GPU stops responding during the frame last handed over. */
PLAT_COLD static void save_gpu_hang(void) {
    const gxCmdQueue_s *queue = &__C3D_Context.gxQueue;
    const FrameSlot *slot = sOnGpu >= 0 ? &sSlots[sOnGpu] : NULL;
    const u32 *base = slot != NULL ? slot->cmd : NULL;
    char path[256];
    char *text;
    int size = 64 * 1024 + 48 * 1024, length = 0, i, stuckEntry = -1;
    u32 listStart = 0, listEnd = 0, words = 0;
    FILE *f;

    if (sHangSaved || slot == NULL) {
        return;
    }
    sHangSaved = 1;
    text = malloc(size);
    if (text == NULL) {
        return;
    }
#define ADD(...)                                                                                                       \
    do {                                                                                                               \
        if (length < size) {                                                                                           \
            length += snprintf(text + length, size - length, __VA_ARGS__);                                            \
        }                                                                                                              \
    } while (0)
    ADD("GPU not responding after frame %d (%d draws, %d triangles, AA %s, %s)\n\n", slot->frame, slot->numDraws,
        sLastFrameTriangles, aa_name(plat_antialias()),
        sOverlap ? "built while the GPU drew the frame before" : "built with the GPU idle");
    // The queue's entries before lastEntry are done; GX runs them in order, so the first one after is where it stopped.
    ADD("GPU command queue: %u entries, %u sent, %u done (the first not done is marked)\n", queue->numEntries,
        queue->curEntry, queue->lastEntry);
    for (i = 0; i < queue->numEntries && queue->entries != NULL; i++) {
        const u32 *data = queue->entries[i].data;
        const char *mark = (i == queue->lastEntry) ? " <-" : "";
        if ((data[0] & 0xFF) == 0x01) {
            u32 start = (u32) ((const u32 *) data[1] - base), end = start + data[2] / 4;
            ADD("  %2d: command list, words %lu to %lu%s\n", i, (unsigned long) start, (unsigned long) end, mark);
            if (end > words && end <= sCmdWords) {
                words = end;
            }
            if (i == queue->lastEntry) {
                stuckEntry = i;
                listStart = start;
                listEnd = end;
            }
        } else {
            ADD("  %2d: GX command %lu (%08lX %08lX %08lX)%s\n", i, (unsigned long) (data[0] & 0xFF),
                (unsigned long) data[1], (unsigned long) data[2], (unsigned long) data[3], mark);
        }
    }
    ADD("\nDraws (cmd = word offsets in gpu_hang_cmd.bin; cc = combiner rgb/alpha per cycle; tex = GPU size, format, "
        "address; '*' = in the command list the GPU stopped in)\n");
    for (i = 0; i < slot->numDraws && i < HANG_DRAWS; i++) {
        const HangDraw *d = &slot->draws[i];
        const GfxDrawState *s = (const GfxDrawState *) d->state; // Only the fields before the textures are there.
        int inStuck = stuckEntry >= 0 && d->cmdEnd > listStart && d->cmdStart < listEnd;
        int t;
        ADD("%c%4d cmd %5lu-%5lu tris %3u blend %u at %u/%u z %u/%u dec %u fog %u cull %u cyc %u", inStuck ? '*' : ' ',
            i, (unsigned long) d->cmdStart, (unsigned long) d->cmdEnd, d->triangles, s->blend, s->alphaTest,
            s->alphaRef, s->depthTest, s->depthWrite, s->decal, s->fogEnabled, s->cullMode, s->combiner.numCycles);
        for (t = 0; t < s->combiner.numCycles && t < 2; t++) {
            const GfxCCEquation *rgb = &s->combiner.rgb[t], *alpha = &s->combiner.alpha[t];
            ADD(" cc %u,%u,%u,%u/%u,%u,%u,%u", rgb->a, rgb->b, rgb->c, rgb->d, alpha->a, alpha->b, alpha->c,
                alpha->d);
        }
        for (t = 0; t < 2; t++) {
            if (d->tex[t] != NULL) {
                ADD(" tex%d %ux%u f%u %p", t, d->tex[t]->width, d->tex[t]->height, (unsigned) d->tex[t]->fmt,
                    d->tex[t]->data);
            }
        }
        ADD("\n");
    }
    if (slot->numDraws > HANG_DRAWS) {
        ADD("(%d more draws not noted)\n", slot->numDraws - HANG_DRAWS);
    }
    ADD("\nRecent log:\n");
    if (length < size) {
        length += plat_log_recent(text + length, size - length);
    }
#undef ADD
    if (length > size - 1) {
        length = size - 1;
    }
    plat_snprintf(path, sizeof(path), "%s/gpu_hang.txt", plat_data_dir());
    plat_save_file(path, text, length);
    free(text);

    plat_snprintf(path, sizeof(path), "%s/gpu_hang_cmd.bin", plat_data_dir());
    f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(base, 4, words, f);
        fclose(f);
    }
    if (stuckEntry >= 0) {
        plat_log("GPU hang saved to gpu_hang.txt: stopped in queue entry %d, command list words %lu to %lu",
                 stuckEntry, (unsigned long) listStart, (unsigned long) listEnd);
    } else {
        plat_log("GPU hang saved to gpu_hang.txt: stopped at queue entry %u of %u", queue->lastEntry,
                 queue->numEntries);
    }
}

/* Logs every draw of the current frame. */
PLAT_COLD void gfxb_log_draws(int snapshotFrame) {
    sLogFrame = sFrameCount;
    sSnapshotFrame = snapshotFrame;
    sSnapshotPending = snapshotFrame >= 0;
    if (sSnapshotPending) {
        sRecentFrozen = 1; // Keep the frames from before the snapshot.
        log_buffers();
    }
}

/* Called on the main thread when the application closes, once the renderer has stopped with the GPU idle. */
void gfxb_3ds_shutdown(void) {
    C3D_Fini();
}

static void delete_mesh(void *mesh);

/* Deletes the textures (and meshes) freed while the slot's frame was built, once the GPU has drawn it. */
static void process_pending_frees(FrameSlot *slot) {
    int i;
    for (i = 0; i < slot->numFrees; i++) {
        texture_unlink(slot->frees[i]);
        if (!slot->frees[i]->shared) {
            C3D_TexDelete(&slot->frees[i]->tex);
        }
        free(slot->frees[i]);
    }
    slot->numFrees = 0;
    for (i = 0; i < slot->numMeshFrees; i++) {
        delete_mesh(slot->meshFrees[i]);
    }
    slot->numMeshFrees = 0;
}

static int overlap_setting(void) {
    static int sSetting = -1;
    if (sSetting < 0) {
        const char *env = plat_getenv("DKR_OVERLAP");
        sSetting = env == NULL || atoi(env) != 0;
        plat_log("frames built while the GPU draws the last one: %s",
                 !sSetting ? "no (DKR_OVERLAP=0)" : sNumSlots < 2 ? "no (one slot)" : "yes");
    }
    return sSetting && sNumSlots == 2;
}

/* Renderer thread, after apply_screen_mode: the target for the frame about to be built (see sAdaptive). */
static void adaptive_pick(void) {
    if (!sAdaptive || sAdaptHold <= 0 || sRenderScale != 2 || sStereo || sSnapshotPending || sTarget == NULL) {
        return;
    }
    if (sLowTarget == NULL) {
        if (sLowFailed) {
            return;
        }
        sLowTarget = new_target(TOP_HEIGHT, TOP_WIDTH * (s800 ? 2 : 1));
        if (sLowTarget == NULL) {
            sLowFailed = 1; // Until the screen mode changes.
            plat_log("adaptive anti-aliasing: no VRAM for the low resolution target");
            return;
        }
        sLowFresh = 1;
    }
    sMainTarget = sTarget;
    sMainScale = sRenderScale;
    sMainScaleX = sScaleX;
    sTarget = sLowTarget;
    sRenderScale = 1;
    sScaleX = s800 ? 2 : 1;
    sLowActive = 1;
    if (sLowFresh) {
        sLowFresh = 0;
        sFrameStarted = 0; // Cleared like a new target.
    }
}

/* Just before a frame is handed over, inside citro3d's frame: the screen shows the frame's target. */
static void adaptive_link(FrameSlot *slot) {
    slot->adaptAa = !sAdaptive ? 0 : sLowActive ? 2 : (sRenderScale == 2 && !sStereo) ? 1 : 0;
    if (sLinked != sTarget) {
        link_target();
    }
}

/* Once the GPU has drawn a frame: how long it took (ms) decides the next frames' resolution (see sAdaptive). */
static void adaptive_observe(const FrameSlot *slot, float gpuMs) {
    float budget;
    if (slot->frame == sAdaptLastFrame) {
        return; // Already seen (wait_for_gpu again with nothing new handed over).
    }
    sAdaptLastFrame = slot->frame;
    if (!sAdaptive) {
        sAdaptHold = 0;
        sAdaptHoldLen = ADAPT_HOLD;
        sAdaptProbe = 0;
        return;
    }
    if (slot->adaptAa == 0) {
        return;
    }
    budget = 1000.0f / 60.0f * (float) plat_frame_retraces();
    sAdaptSeen++;
    if (slot->adaptAa == 2) {
        sAdaptLow++;
        if (sAdaptHold > 0 && --sAdaptHold == 0) {
            sAdaptProbe = ADAPT_PROBE;
        }
    } else if (gpuMs > budget * 0.92f || sAdaptive == 2) {
        sAdaptSlow++;
        if (sAdaptProbe > 0 && sAdaptHoldLen < ADAPT_HOLD_MAX) {
            sAdaptHoldLen *= 2; // Slow again straight after a hold: hold longer.
        }
        sAdaptProbe = 0;
        sAdaptHold = sAdaptHoldLen;
    } else if (sAdaptProbe > 0 && --sAdaptProbe == 0) {
        sAdaptHoldLen = ADAPT_HOLD; // Anti-aliased frames are fast enough again.
    }
    if (sAdaptSeen >= 600) {
        plat_log("adaptive anti-aliasing: %d of the last %d frames at the low resolution, %d slow anti-aliased "
                 "frames, hold %d frames",
                 sAdaptLow, sAdaptSeen, sAdaptSlow, sAdaptHoldLen);
        sAdaptSeen = sAdaptLow = sAdaptSlow = 0;
    }
}

/*
 * Waits for the GPU to finish the frame it was given last, and begins a citro3d frame (which sets citro3d's own
 * command buffer: the caller sets the slot's). Then that frame's freed textures are deleted.
 */
static void wait_for_gpu(int measure) {
    unsigned long long waitStart;
    if (sFrameCount < 3) {
        plat_log("frame %d: waiting for the GPU", sFrameCount);
    }
    gPlatGfxStage = "waiting for the GPU";
    waitStart = plat_time_ns();
    if (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        // Poll instead of blocking, so that a GPU that never finishes the previous frame gets reported.
        unsigned long long start = plat_time_ns();
        int reported = 0;
        while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
            if (!reported && plat_time_ns() - start > 3000000000ULL) {
                reported = 1;
                gPlatGfxStage = "GPU not responding";
                plat_status("GPU stuck after frame %d (%d triangles, cmd buffer %.0f%%)",
                            sOnGpu >= 0 ? sSlots[sOnGpu].frame : -1, sLastFrameTriangles, sLastFrameUsage * 100.0f);
                save_gpu_hang();
            }
            plat_3ds_quit_point(PLAT_3DS_PARKED_GPU_BUSY);
            plat_sleep_ns(250000);
        }
    }
    if (sOnGpu >= 0) {
        transform_frame_drawn(sSlots[sOnGpu].frame);
        adaptive_observe(&sSlots[sOnGpu], C3D_GetDrawingTime());
    }
    if (measure && prof_enabled()) {
        prof_add(PROF_GPU_WAIT, plat_time_ns() - waitStart);
        if (sOnGpu >= 0) {
            // How long the GPU took to draw the previous frame, which it has just finished.
            prof_add(PROF_GPU_DRAW, (unsigned long long) (C3D_GetDrawingTime() * 1000000.0f));
        }
    }
    if (sSnapshotPending && sOnGpu >= 0 && sSlots[sOnGpu].frame == sLogFrame) {
        // The snapshot's log file is closed here, once the GPU has drawn the frame and its textures are checked.
        sSnapshotPending = 0;
        check_textures_after_frame();
        save_recent_frames(sSnapshotFrame);
        sRecentFrozen = 0;
        plat_log_copy_to(NULL);
        plat_status("Snapshot saved (frame %d)", sSnapshotFrame);
        gPlatSnapshotSaved = sSnapshotFrame;
    }
    if (sFrameCount < 3) {
        plat_log("frame %d: GPU ready", sFrameCount);
    }
    if (sFrameCount == 1) {
        plat_status("First frame drawn");
    }
    if (sOnGpu >= 0) {
        process_pending_frees(&sSlots[sOnGpu]);
    }
}

/******************************** GPU vertex maths ********************************/

/*
 * GPU vertex maths (roadmap item P30, DKR_GPU_TRANSFORM=1): every draw goes through transform.v.pica instead of the
 * pass-through shader. The frontend hands most 3D triangles over as the game's own vertices with their matrix
 * (gfxb_draw_model, gfxb_draw_meshes), and the GPU transforms them; the rest still arrive transformed by the CPU
 * (gfxb_draw_indexed), drawn by the same shader with an identity matrix (set_cpu_uniforms).
 *
 * The first shader that transformed on the GPU hung a real New 3DS on its first triangle (2026-10-16), while
 * emulators ran it. So the first frames drawn with it are watched: gpu_transform.txt says "testing" until 30 frames
 * have been drawn, then "passed". If the console hangs meanwhile, the next start finds "testing", writes "hung" and
 * leaves GPU vertex maths off (DKR_GPU_TRANSFORM is ignored) until TRANSFORM_TEST_VERSION changes with a new shader.
 */
#define TRANSFORM_TEST_VERSION 1
#define TRANSFORM_TEST_FRAMES 30

static shaderProgram_s sProgramT;
static int sProgramTUsable;  // transform.v.pica's uniforms were all found
static int sTransformHung;   // It hung the GPU in an earlier run (gpu_transform.txt)
static int sTransformWanted; // DKR_GPU_TRANSFORM
static int sTransformOn;     // This frame is drawn with transform.v.pica
static int sTransformTestFrame = -1; // The first frame drawn with it this run, until it is known to work
static int sEyeUniformP, sEyeUniformT; // The eye uniform of each program

enum { UNIF_MTX, UNIF_CAMOFF = 4, UNIF_TEXA, UNIF_TEXC, UNIF_TEXB, UNIF_FOGP, UNIF_COUNT };
static int sUnifLoc[UNIF_COUNT];
static float sUnifVal[UNIF_COUNT][4];
static int sUnifKnown; // sUnifVal holds what citro3d has
static int sUnifModel; // The uniforms are a model draw's (else the CPU draws' identity)
static unsigned sUnifChanged; // Uniforms set since they were last written (bit per UNIF_*)
static int sUnifTwice;        // DKR_UNIF_TWICE: changed uniforms written twice (unif_write_twice)

static void transform_marker_path(char *out, size_t size) {
    snprintf(out, size, "%s/gpu_transform.txt", plat_data_dir());
}

static void transform_marker_write(const char *what) {
    char path[256], text[32];
    transform_marker_path(path, sizeof(path));
    snprintf(text, sizeof(text), "%s %d\n", what, TRANSFORM_TEST_VERSION);
    plat_save_file(path, text, strlen(text));
}

/* At start-up: finds the transform program's uniforms and reads what happened in earlier runs. */
static void init_transform(void) {
    static const char *const sNames[] = { "mtx", "camoff", "texa", "texc", "texb", "fogp" };
    char path[256], text[32];
    int i, got;

    shaderProgramInit(&sProgramT);
    sProgramTUsable = sShaderDvlb->numDVLE >= 2 && shaderProgramSetVsh(&sProgramT, &sShaderDvlb->DVLE[1]) == 0;
    sEyeUniformP = sEyeUniform;
    sEyeUniformT = sProgramTUsable ? shaderInstanceGetUniformLocation(sProgramT.vertexShader, "eye") : -1;
    for (i = 0; i < UNIF_COUNT; i++) {
        int name = i < UNIF_CAMOFF ? 0 : i - UNIF_CAMOFF + 1;
        int loc = sProgramTUsable ? shaderInstanceGetUniformLocation(sProgramT.vertexShader, sNames[name]) : -1;
        sUnifLoc[i] = loc >= 0 && i < UNIF_CAMOFF ? loc + i : loc;
        if (sUnifLoc[i] < 0) {
            sProgramTUsable = 0;
        }
    }
    if (sEyeUniformT < 0) {
        sProgramTUsable = 0;
    }
    plat_log("GPU vertex maths: shader %s, uniforms eye %d mtx %d camoff %d texa %d texc %d texb %d fogp %d",
             sProgramTUsable ? "found" : "NOT usable", sEyeUniformT, sUnifLoc[UNIF_MTX], sUnifLoc[UNIF_CAMOFF],
             sUnifLoc[UNIF_TEXA], sUnifLoc[UNIF_TEXC], sUnifLoc[UNIF_TEXB], sUnifLoc[UNIF_FOGP]);

    transform_marker_path(path, sizeof(path));
    got = plat_load_file_start(path, text, sizeof(text) - 1);
    if (got > 0) {
        text[got] = '\0';
        if ((strncmp(text, "testing ", 8) == 0 && atoi(text + 8) == TRANSFORM_TEST_VERSION) ||
            (strncmp(text, "hung ", 5) == 0 && atoi(text + 5) == TRANSFORM_TEST_VERSION)) {
            sTransformHung = 1;
            transform_marker_write("hung");
            plat_log("GPU vertex maths: left off, the GPU hung with it in an earlier run (gpu_transform.txt)");
        }
    }
}

/* Writes the uniform if it changed (through citro3d, which writes it again after the HOME menu). */
static void set_unif(int u, float x, float y, float z, float w) {
    float *c = sUnifVal[u];
    if (sUnifKnown && c[0] == x && c[1] == y && c[2] == z && c[3] == w) {
        return;
    }
    c[0] = x;
    c[1] = y;
    c[2] = z;
    c[3] = w;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sUnifLoc[u], x, y, z, w);
    sUnifChanged |= 1u << u;
}

/*
 * The GPU misses some settings written right after a draw (see drain_before_draw), and a model draw changes the
 * matrix uniforms right after the CPU draw before it. Missed, the draw lands with the identity matrix, nowhere on the
 * screen: a tunnel's walls and ceiling vanished at one spot of a track with GPU vertex maths on (bug report #234),
 * the reference renderer drawing them from the same draw. So the changed uniforms are written now and once more with
 * the draw, as write_settings_twice does for the other settings. DKR_UNIF_TWICE=0 writes them once.
 */
static void PLAT_COLD unif_write_twice_now(void) {
    unsigned changed = sUnifChanged;
    int u;
    sUnifChanged = 0;
    if (!sUnifTwice) {
        return;
    }
    C3Di_UpdateContext();
    for (u = 0; u < UNIF_COUNT; u++) {
        if (changed & (1u << u)) {
            C3D_FVUnifDirty[GPU_VERTEX_SHADER][sUnifLoc[u]] = true;
        }
    }
}

/* Only GPU vertex maths (P30) changes uniforms between draws: the test stays in the draws, the work out of their way
 * (it would put the per-draw code past the Old 3DS's instruction cache). */
static inline void unif_write_twice(void) {
    if (__builtin_expect(sUnifChanged != 0, 0)) {
        unif_write_twice_now();
    }
}

/* The uniforms for a draw the CPU transformed: the shader passes it through, as shader.v.pica does. */
static void PLAT_COLD set_cpu_uniforms_now(void) {
    set_unif(UNIF_MTX + 0, 1.0f, 0.0f, 0.0f, 0.0f);
    set_unif(UNIF_MTX + 1, 0.0f, 1.0f, 0.0f, 0.0f);
    set_unif(UNIF_MTX + 2, 0.0f, 0.0f, 1.0f, 0.0f);
    set_unif(UNIF_MTX + 3, 0.0f, 0.0f, 0.0f, 1.0f);
    set_unif(UNIF_CAMOFF, 0.0f, 0.0f, 0.0f, 0.0f);
    set_unif(UNIF_TEXA, 1.0f, 1.0f, 0.0f, 0.0f);
    set_unif(UNIF_TEXC, 0.0f, 0.0f, 1.0f, 1.0f);
    set_unif(UNIF_TEXB, 0.0f, 0.0f, 0.0f, 0.0f);
    set_unif(UNIF_FOGP, 0.0f, 0.0f, 0.0f, 0.0f);
    sUnifKnown = 1;
    sUnifModel = 0;
}

static inline void set_cpu_uniforms(void) {
    if (__builtin_expect(sTransformOn && !(sUnifKnown && !sUnifModel), 0)) {
        set_cpu_uniforms_now();
    }
}

/*
 * The uniforms for a model draw: its matrix with what to_screen does folded in (the quarter turn, the 3D width, the
 * depth remap; the clamp is the shader's), and its texture factors with the padded size and flipped t of
 * write_vertices.
 *
 * The GPU works in 24-bit floats, with 16 bits after the point. A track's matrix takes the camera's position off
 * coordinates in the thousands, and doing that inside the matrix loses the low bits: the nearby world would shake by
 * pixels. So, as the F-Zero X 3DS port does, the whole-number part of the camera position (the model space point the
 * matrix takes to x = y = w = 0) is taken off each vertex first, which is exact for whole numbers this size, and the
 * matrix only keeps what is left of its offset.
 */
static void set_model_uniforms(const GfxModelParams *p, float su0, float sv0, float su1, float sv1) {
    double rows[4][4], cam[3] = { 0.0, 0.0, 0.0 }, a[3][3], b[3], det;
    static const int sCols[3] = { 0, 1, 3 };
    int r, k;

    for (r = 0; r < 4; r++) {
        rows[0][r] = p->mtx[r][1];
        rows[1][r] = -(double) p->mtx[r][0] * sScale3D;
        rows[2][r] = 0.5 * ((double) p->mtx[r][2] - p->mtx[r][3]);
        rows[3][r] = p->mtx[r][3];
    }
    for (k = 0; k < 3; k++) {
        for (r = 0; r < 3; r++) {
            a[k][r] = p->mtx[r][sCols[k]];
        }
        b[k] = -(double) p->mtx[3][sCols[k]];
    }
    det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
          a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (det > 1e-30 || det < -1e-30) {
        for (k = 0; k < 3; k++) {
            double m[3][3];
            int i, j;
            for (i = 0; i < 3; i++) {
                for (j = 0; j < 3; j++) {
                    m[i][j] = j == k ? b[i] : a[i][j];
                }
            }
            cam[k] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                      m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) / det;
            cam[k] = cam[k] < -60000.0 ? -60000.0 : cam[k] > 60000.0 ? 60000.0 : floor(cam[k] + 0.5);
        }
    }
    for (k = 0; k < 4; k++) {
        double t = rows[k][3] + cam[0] * rows[k][0] + cam[1] * rows[k][1] + cam[2] * rows[k][2];
        set_unif(UNIF_MTX + k, (float) rows[k][0], (float) rows[k][1], (float) rows[k][2], (float) t);
    }
    set_unif(UNIF_CAMOFF, (float) cam[0], (float) cam[1], (float) cam[2], 0.0f);
    set_unif(UNIF_TEXA, p->texScale[0] * su0, -p->texScale[1] * sv0, p->texScale[2] * su1, -p->texScale[3] * sv1);
    set_unif(UNIF_TEXC, 0.0f, 0.0f, 0.0f, 0.0f);
    set_unif(UNIF_TEXB, p->texOffset[0] * su0, 1.0f - p->texOffset[1] * sv0, p->texOffset[2] * su1,
             1.0f - p->texOffset[3] * sv1);
    if (p->fog) {
        // The shader has z' = (z - w) / 2, so z / w = 2 z' / w + 1.
        set_unif(UNIF_FOGP, 2.0f * p->fogMul / 255.0f, (p->fogMul + p->fogOffset) / 255.0f, 1.0f, 0.0f);
    } else {
        set_unif(UNIF_FOGP, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    sUnifKnown = 1;
    sUnifModel = 1;
}

/* At the start of a frame: binds the program the frame is drawn with, and watches the first frames drawn with the
 * transform one (see above). */
static void transform_frame_start(void) {
    int on = sTransformWanted && sProgramTUsable && !sTransformHung;
    if (on != sTransformOn) {
        sTransformOn = on;
        C3D_BindProgram(on ? &sProgramT : &sProgram);
        sEyeUniform = on ? sEyeUniformT : sEyeUniformP;
        sUnifKnown = 0;
        plat_log("GPU vertex maths %s (frame %d)", on ? "on" : "off", sFrameCount);
        if (on && sTransformTestFrame == -1) {
            transform_marker_write("testing");
            sTransformTestFrame = sFrameCount;
        }
    }
}

/* Once the GPU has drawn a frame: marks the transform program as working after its first frames. */
static void transform_frame_drawn(int frame) {
    if (sTransformTestFrame >= 0 && frame >= sTransformTestFrame + TRANSFORM_TEST_FRAMES) {
        sTransformTestFrame = -2;
        transform_marker_write("passed");
        plat_log("GPU vertex maths: %d frames drawn, the shader works on this console", TRANSFORM_TEST_FRAMES);
    }
}

/*
 * Whether the walk may drop back faces itself (P40). Not in 3D, where each eye's offset can turn a nearly edge-on
 * triangle round, and not with DKR_NO_CULL. With the two-core renderer sStereo is the backend's, up to two frames
 * behind, so the slider is read here too: while split, a raised slider stops the walk culling before the backend
 * turns 3D on.
 */
int gfxb_cpu_cull_ok(void) {
    static int noCull = -1;

    if (noCull < 0) {
        noCull = plat_getenv("DKR_NO_CULL") != NULL;
    }
    return !noCull && !sStereo && (!gfx_split_active() || osGet3DSliderState() <= 0.0f);
}

int gfxb_gpu_transform(void) {
    return sTransformOn;
}

/*
 * The experimental switches the switch bench (roadmap P29) flips between races, read again at the start of a frame
 * when one changes: DKR_GPU_SPLIT (command list splits before blended draws, P19; 0 off, the default, 1 before
 * blending, 2 on every state change; see drain_before_draw), DKR_LINE_WATCH and DKR_RECENT_FRAMES (the white-line
 * diagnostics, P28), DKR_EARLY_DEPTH (the early depth test, P20), DKR_SVC_FLUSH (cache flushes as a system call,
 * P24, see gpu_flush) and DKR_DEPTH_FILL (the depth clear as a memory fill, P18, on by default, see
 * gfxb_clear_depth). DKR_TEX_VRAM (P17) is read when a texture is made instead, as the switch bench sets it while a
 * level loads its textures.
 */
static int sSwitchEpoch = -1;
static int sSplitMode, sEarlyDepth, sDepthFill;
static int sDrawLean; // DKR_DRAW_LEAN: the one-pass draw, see gfxb_draw_indexed
static void set_frame_band(void);
static int sFastState = 1; // DKR_FAST_STATE (roadmap P23): the cheaper per-draw settings path
static int sFastClip = 1;  // DKR_FAST_CLIP (roadmap P22): triangles wholly beyond the band dropped, not clipped
static int sDrawParts;     // DKR_DRAW_PARTS: time each draw's parts while the profiler is on (gfxb_draw_indexed)

static void read_switches(void) {
    int recent, i;
    if (sSwitchEpoch == gPlatSwitchEpoch) {
        return;
    }
    sSwitchEpoch = gPlatSwitchEpoch;
    sSplitMode = plat_switch("DKR_GPU_SPLIT", 0);
    sLineWatch = plat_switch("DKR_LINE_WATCH", 0);
    sEarlyDepth = plat_switch("DKR_EARLY_DEPTH", 0) != 0;
    // On by default on an Old 3DS only (switch bench #241: with the renderer on the system core the GPU service's
    // flushes are slow there). The New 3DS's #176 showed no difference.
    sSvcFlush = plat_switch("DKR_SVC_FLUSH", plat_old_3ds()) != 0;
    sDepthFill = plat_switch("DKR_DEPTH_FILL", 1) != 0;
    sFastState = plat_switch("DKR_FAST_STATE", 1) != 0;
    sFastClip = plat_switch("DKR_FAST_CLIP", 1) != 0;
    sTripleWanted = plat_switch("DKR_TRIPLE_BUFFER", !plat_old_3ds()) != 0;
    // The lean draw (gfxb_draw_indexed): on by default on an Old 3DS only, until a New 3DS bench has measured it.
    sDrawLean = plat_switch("DKR_DRAW_LEAN", plat_old_3ds()) != 0;
    // Off on an Old 3DS: the five to seven clock reads a draw are system calls there (see the draws' parts below).
    sDrawParts = plat_switch("DKR_DRAW_PARTS", !plat_old_3ds()) != 0;
    // GPU vertex maths (P30) and stereo frames made once (P33): Settings > Picture rows, off by default.
    sTransformWanted = plat_switch("DKR_GPU_TRANSFORM", plat_gpu_maths_setting() >= 1) != 0;
    sUnifTwice = plat_switch("DKR_UNIF_TWICE", 1) != 0;
    sStereoOnce = plat_switch("DKR_STEREO_ONCE", plat_stereo_once_setting()) != 0;
    sAdaptive = plat_switch("DKR_ADAPTIVE_AA", 0);
    recent = plat_switch("DKR_RECENT_FRAMES", 0) != 0;
    if (recent && sRecentCount == 0) {
        allocate_recent_frames();
    }
    if (recent != sRecentOn) {
        for (i = 0; i < sRecentCount; i++) {
            sRecentFrame[i] = -1; // Old pictures, from before it was turned off.
        }
        sRecentOn = recent;
    }
    plat_log("switches: command list split %s, line watch %d, recent frames %s (%d slots), early depth test %s, "
             "settings path %s",
             sSplitMode == 0 ? "off" : sSplitMode == 1 ? "before blending" : "on state changes", sLineWatch,
             sRecentOn ? "on" : "off", sRecentCount, sEarlyDepth ? "on" : "off", sFastState ? "short" : "full");
    plat_log("switches: cache flush by %s, depth clear by %s, textures in VRAM %s, edge trimming %s, draws timed in "
             "parts %s",
             sSvcFlush ? "system call" : "GSP service", sDepthFill ? "memory fill" : "drawing",
             plat_switch("DKR_TEX_VRAM", 0) ? "on" : "off", sFastClip ? "drops what is wholly outside" : "clips all",
             sDrawParts ? "on" : "off");
    plat_log("switches: draw path %s", sDrawLean ? "lean (vertices tested and written in one pass)" : "full");
    plat_log("switches: GPU vertex maths %s%s, stereo frames %s", sTransformWanted ? "on" : "off",
             sTransformHung ? " (but it hung before: left off)" : !sProgramTUsable ? " (shader not usable)" : "",
             sStereoOnce ? "made once, drawn twice" : "made for each eye");
    plat_log("switches: adaptive anti-aliasing %s, top screen buffers %d, queue wake-ups %s", sAdaptive ? "on" : "off",
             sTripleWanted ? 3 : 2, plat_switch("DKR_QUEUE_WAKE", 0) ? "per queue" : "all threads");
}

void gfxb_start_frame(void) {
    FrameSlot *slot;
    // No vblank throttling here: the emulated VI already paces the game.
    capture_pending_frame(); // Outside a frame, so the copy waits for the GPU to finish the previous one.
    apply_screen_mode();
    adaptive_pick();
    // The first frame of a render target clears it with the GPU idle, and a snapshot frame is checked once the GPU
    // has drawn it, before the next is built: those wait for the GPU first, as every frame used to.
    sOverlap = overlap_setting() && sFrameStarted < 0 && !sSnapshotPending;
    if (!sOverlap) {
        wait_for_gpu(1);
        // The application is closing or suspended: stop here, with the GPU idle and no frame being built.
        plat_3ds_quit_point(PLAT_3DS_PARKED_GPU_IDLE);
    }
    gPlatGfxStage = "drawing";

    slot = &sSlots[sRec];
    slot->numParts = 0;
    slot->numDraws = 0;
    slot->frame = sFrameCount;
    sVbo = slot->vbo;
    sVboUsed = 0;
    sVboFlushed = 0;
    sIbo = slot->ibo;
    sIboUsed = 0;
    sIboFlushed = 0;
    sVfmtBound = -1; // The first draw points the GPU at this slot's buffer.
    sVfmtSwitches = 0;
    sBufMesh = NULL;
    GPUCMD_SetBuffer(slot->cmd, sCmdWords, 0);
    read_switches();
    transform_frame_start();
    sOnceFrame = sStereo && sStereoOnce && sSplitMode == 0;
    if (!sStereo) {
        write_eye(0.0f, 0.0f); // In 3D, each draw sets it (draw_arrays), or each eye's pass (stereo_once_lists).
    }
    // Like an N64 framebuffer, the colour buffer keeps the last image: the game clears it itself, and a frame that
    // draws nothing (while a level loads) must not show up as a black flash. So only the first frame uses a memory
    // fill; after that the depth buffer is cleared by drawing. (A memory fill of the depth buffer alone never finished
    // on a real New 3DS, which hung the game after its first frame. Emulators complete it.)
    if (sFrameStarted >= 0) {
        C3D_RenderTargetClear(sTarget, C3D_CLEAR_ALL, CLEAR_BLACK, 0);
        if (sStereo) {
            C3D_RenderTargetClear(sTargetRight, C3D_CLEAR_ALL, CLEAR_BLACK, 0);
        }
    }
    // What C3D_FrameDrawOn does, which needs a citro3d frame begun; the target is marked used when handed over.
    C3D_SetFrameBuf(&sTarget->frameBuf);
    if (sOnceFrame) {
        // The frame's commands are run once for each eye, the target and eye offset set before each run
        // (stereo_once_lists). So they must not set the target themselves, and the first draw writes every setting
        // the draws rely on, which the second run would otherwise find as the first left them.
        __C3D_Context.flags = (__C3D_Context.flags & ~C3D_FLAG_FRAMEBUF) | C3D_ONCE_FLAGS;
        C3Di_DirtyUniforms(GPU_VERTEX_SHADER);
    } else {
        bind_target(sTarget); // In case citro3d's frame hasn't begun, or the last frame ended on the right eye's.
    }
    C3D_SetViewport(0, 0, sTarget->frameBuf.width, sTarget->frameBuf.height);
    set_frame_band();
    promote_textures();
    forget_gpu_state();
    // The switch bench sets these itself (BENCH_*), so the player's settings don't change what it measures.
    sGuardBandFrame = plat_3ds_guard_band_setting();
    sMipmapsUsed = plat_switch("BENCH_MIPMAPS", sMipmapSetting) != 0;
    sMipmapsShown = plat_switch("BENCH_MIPMAPS", sMipmapSetting) == 2;
    sColourBoost = plat_switch("BENCH_COLOUR", sColourSetting);
    sFrameFirstDraw = 1;
    sDepthFillPending = 0;
    if (sFrameStarted < 0) {
        static const short sFullScreen[4] = { 0, 0, GFX_SCREEN_WIDTH, GFX_SCREEN_HEIGHT };
        gfxb_clear_depth(sFullScreen);
    }
    sFrameStarted = 1;
}

/* DKR_GFX_TEST draws a reference quad: a 16x16 texture, red on top, blue at the bottom, with a green left column. */
static int sTestMode;
PLAT_COLD static void draw_test_pattern(void) {
    static GfxTexture sTex;
    static unsigned int sPixels[16 * 16];
    GfxDrawState state;
    GfxVertex v[6];
    static const float xs[6] = { -0.9f, -0.9f, -0.1f, -0.1f, -0.9f, -0.1f };
    static const float ys[6] = { 0.9f, 0.1f, 0.9f, 0.9f, 0.1f, 0.1f };
    int i;

    if (sTex.width == 0) {
        for (i = 0; i < 16 * 16; i++) {
            int x = i % 16, y = i / 16;
            sPixels[i] = (x < 2) ? 0xFF00FF00 : (0xFF000000 | (y * 16 + 15));
        }
        sTex.width = 16;
        sTex.height = 16;
        sTex.pixels = sPixels;
        sTex.filter = GFX_FILTER_POINT;
        gfxb_texture_upload(&sTex);
    }
    memset(&state, 0, sizeof(state));
    state.combiner.numCycles = 1;
    state.combiner.rgb[0].a = GFX_CC_ZERO;
    state.combiner.rgb[0].b = GFX_CC_ZERO;
    state.combiner.rgb[0].c = GFX_CC_ZERO;
    state.combiner.rgb[0].d = GFX_CC_TEXEL0;
    state.combiner.alpha[0] = state.combiner.rgb[0];
    if (sTestMode == 3) {
        GfxCCEquation alpha = { GFX_CC_TEXEL0, GFX_CC_ZERO, GFX_CC_PRIM, GFX_CC_ZERO };
        state.combiner.alpha[0] = alpha;
        state.prim[0] = state.prim[1] = state.prim[2] = state.prim[3] = 255;
        state.blend = GFX_BLEND_ALPHA;
    }
    if (sTestMode == 4) {
        GfxCCEquation alpha = { GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_PRIM };
        state.combiner.alpha[0] = alpha;
        state.prim[3] = 255;
        state.blend = GFX_BLEND_ALPHA;
    }
    if (sTestMode == 5) {
        GfxCCEquation alpha = { GFX_CC_TEXEL0, GFX_CC_ZERO, GFX_CC_PRIM, GFX_CC_ZERO };
        state.combiner.alpha[0] = alpha;
        state.prim[3] = 255;
        state.alphaTest = 1;
        state.alphaRef = 128;
    }
    if (sTestMode == 2) {
        // The menu font combiner: lerp(TEXEL0, ENV, ENV_ALPHA), alpha TEXEL0 * PRIM.
        GfxCCEquation rgb = { GFX_CC_ENV, GFX_CC_TEXEL0, GFX_CC_ENV_ALPHA, GFX_CC_TEXEL0 };
        GfxCCEquation alpha = { GFX_CC_TEXEL0, GFX_CC_ZERO, GFX_CC_PRIM, GFX_CC_ZERO };
        state.combiner.rgb[0] = rgb;
        state.combiner.alpha[0] = alpha;
        state.env[0] = state.env[1] = state.env[2] = 255;
        state.env[3] = 0;
        state.prim[0] = state.prim[1] = state.prim[2] = state.prim[3] = 255;
        state.blend = GFX_BLEND_ALPHA;
    }
    state.textures[0] = &sTex;
    state.flat = 1;
    state.scissor[2] = GFX_SCREEN_WIDTH;
    state.scissor[3] = GFX_SCREEN_HEIGHT;
    for (i = 0; i < 6; i++) {
        memset(&v[i], 0, sizeof(v[i]));
        v[i].x = xs[i];
        v[i].y = ys[i];
        v[i].z = 0.0f;
        v[i].w = 1.0f;
        v[i].u0 = (xs[i] + 0.9f) / 0.8f;
        v[i].v0 = (0.9f - ys[i]) / 0.8f;
        v[i].r = v[i].g = v[i].b = v[i].a = 255;
    }
    gfxb_draw_triangles(&state, v, 2);
}

void gfxb_end_frame(void) {
    {
        static int sTest = -1;
        if (sTest < 0) {
            const char *env = plat_getenv("DKR_GFX_TEST");
            sTest = env != NULL;
            sTestMode = env != NULL ? atoi(env) : 0;
        }
        if (sTest) {
            draw_test_pattern();
        }
    }
    if (sFrameCount == sLogFrame && sSnapshotFrame >= 0) {
        save_command_list();
    }
    hand_over_draw_parts();
    if (sOnceFrame) {
        stereo_once_lists(&sSlots[sRec]);
    }
    {
        static float sMaxUsage;
        FrameSlot *slot = &sSlots[sRec];
        u32 *tail = gpuCmdBuf, tailSize = gpuCmdBufSize, tailOffset = gpuCmdBufOffset;
        float usage;
        int i;

        if (full_flush()) {
            gpu_flush(sVbo, sVboUsed);
            if (sIboUsed > 0) {
                gpu_flush(sIbo, sIboUsed * sizeof(u16));
            }
        } else {
            flush_new_vertices();
        }
        slot->words = (u32) (gpuCmdBuf + gpuCmdBufOffset - slot->cmd);
        usage = (float) slot->words / sCmdWords;
        if (usage > sMaxUsage) {
            sMaxUsage = usage;
        }
        if ((sFrameCount++ % 120) == 0) {
            plat_log("frame %d: %d triangles at %llu ms, command buffer %.0f%% (max %.0f%%), %d splits, %d KB of "
                     "vertices, %d format changes", sFrameCount, sFrameTriangles, plat_time_ns() / 1000000ULL,
                     usage * 100.0f, sMaxUsage * 100.0f, sDrainSplits, sVboUsed / 1024, sVfmtSwitches);
        }
        {
            static u32 sLoggedVramBytes;
            if (sVramTextureBytes != sLoggedVramBytes) {
                sLoggedVramBytes = sVramTextureBytes;
                plat_log("textures in VRAM: %lu KB, %lu KB of VRAM free", (unsigned long) (sVramTextureBytes / 1024),
                         (unsigned long) (vramSpaceFree() / 1024));
            }
        }
        if (sOverlap) {
            // Built while the GPU drew the last frame: wait for it now, then hand this one over.
            unsigned long long start = prof_enabled() ? plat_time_ns() : 0;
            wait_for_gpu(1);
            if (start != 0) {
                prof_part(PROF_PART_END_GPU_WAIT, plat_time_ns() - start);
            }
        }
        // C3D_FrameBegin set citro3d's own command buffer: carry on in the slot's, where the frame is.
        GPUCMD_SetBuffer(tail, tailSize, tailOffset);
        if (sDepthFillPending) {
            // With the GPU idle: the fill runs before the frame's command list in the GPU queue.
            C3D_RenderTargetClear(sTarget, C3D_CLEAR_DEPTH, 0, 0);
            if (sStereo) {
                C3D_RenderTargetClear(sTargetRight, C3D_CLEAR_DEPTH, 0, 0);
            }
        }
        for (i = 0; i < slot->numParts; i++) {
            if (sSvcFlush) {
                gpu_flush(slot->partAddr[i], slot->partWords[i] * 4);
                GX_ProcessCommandList(slot->partAddr[i], slot->partWords[i] * 4, 0);
            } else {
                GX_ProcessCommandList(slot->partAddr[i], slot->partWords[i] * 4, GX_CMDLIST_FLUSH);
            }
        }
        adaptive_link(slot);
        sTarget->used = true;
        if (sStereo) {
            sTargetRight->used = true;
        }
        sLastFrameTriangles = sFrameTriangles;
        sLastFrameUsage = usage;
        sFrameTriangles = 0;
        sLastDrainSplits = sDrainSplits;
        sDrainSplits = 0;
        sOnGpu = sRec;
        sRec = (sRec + 1) % sNumSlots;
        pace_frame();
        if (!triple_choose()) {
            wait_for_present();
        }
        find_recent_screen();
        watch_for_line();
        sTripleGet = sTripleFrame; // citro3d's display transfer goes to our buffer (see sTripleFrame)
        if (full_flush()) {
            C3D_FrameEnd(0); // Also flushes the whole linear heap.
        } else {
            C3D_FrameEnd(GX_CMDLIST_FLUSH);
        }
        sTripleGet = 0;
        record_recent_frame();
    }
    sFrameStarted = -1; // A frame has been drawn, so the target holds an image.
    if (sOverlap && plat_3ds_renderer_should_park()) {
        // The application is closing or being suspended: stop with the GPU idle. The HOME menu changes the GPU's
        // settings, so no frame built before it may be handed over after it (citro3d rewrites them all afterwards).
        wait_for_gpu(0);
        plat_3ds_quit_point(PLAT_3DS_PARKED_GPU_IDLE);
        C3D_FrameEnd(GX_CMDLIST_FLUSH); // Empty: ends the frame wait_for_gpu began.
    }
}

/******************************** Textures ********************************/

static const u8 sTileOrder[64] = { 0,  1,  4,  5,  16, 17, 20, 21, 2,  3,  6,  7,  18, 19, 22, 23,
                                   8,  9,  12, 13, 24, 25, 28, 29, 10, 11, 14, 15, 26, 27, 30, 31,
                                   32, 33, 36, 37, 48, 49, 52, 53, 34, 35, 38, 39, 50, 51, 54, 55,
                                   40, 41, 44, 45, 56, 57, 60, 61, 42, 43, 46, 47, 58, 59, 62, 63 };

static unsigned int next_pot(unsigned int v) {
    unsigned int p = 8;
    while (p < v && p < 1024) {
        p <<= 1;
    }
    return p;
}

static GPU_TEXTURE_WRAP_PARAM wrap_param(unsigned char mode) {
    switch (mode) {
        case GFX_WRAP_MIRROR:
            return GPU_MIRRORED_REPEAT;
        case GFX_WRAP_CLAMP:
            return GPU_CLAMP_TO_EDGE;
        default:
            return GPU_REPEAT;
    }
}

/*
 * The GPU format a texture is stored in: the frontend's pick (16 or 8 bits a texel for most of DKR's textures), or
 * RGBA8 for all of them with DKR_TEX_NATIVE=0.
 */
static int gpu_format(const GfxTexture *tex) {
    static int sNative = -1;
    if (sNative < 0) {
        const char *env = plat_getenv("DKR_TEX_NATIVE");
        sNative = env == NULL || env[0] != '0';
    }
    return sNative ? tex->format : GFX_TEXFMT_RGBA8;
}

static GPU_TEXCOLOR gpu_texcolor(int format) {
    switch (format) {
        case GFX_TEXFMT_RGBA5551:
            return GPU_RGBA5551;
        case GFX_TEXFMT_LA8:
            return GPU_LA8;
        case GFX_TEXFMT_LA4:
            return GPU_LA4;
        default:
            return GPU_RGBA8;
    }
}

/*
 * The texel at (px, py) of the power of two sized texture, as RGBA8888. Wrapping textures that aren't a power of two
 * are stretched to fit, others are padded (repeating their edge texels) and their coordinates scaled.
 */
static u32 pot_texel(const GfxTexture *tex, unsigned int pw, unsigned int ph, int resample, unsigned int px,
                     unsigned int py) {
    unsigned int w = tex->width, h = tex->height;
    unsigned int sx, sy;
    if (resample) {
        sx = px * w / pw;
        sy = py * h / ph;
    } else {
        sx = (px < w) ? px : w - 1;
        sy = (py < h) ? py : h - 1;
    }
    return tex->pixels[sy * w + sx];
}

/* The same texel in the GPU format's bits. */
static u32 gpu_texel(const GfxTexture *tex, int format, unsigned int pw, unsigned int ph, int resample,
                     unsigned int px, unsigned int py) {
    return gfx_texel_encode(pot_texel(tex, pw, ph, resample, px, py), format);
}

/* Writes texel i of an 8x8 tile (in the GPU's order) to a texture of the given format. */
static inline void put_texel(void *tile, int format, int i, u32 v) {
    switch (gfx_texel_bytes(format)) {
        case 4:
            ((u32 *) tile)[i] = v;
            break;
        case 2:
            ((u16 *) tile)[i] = v;
            break;
        default:
            ((u8 *) tile)[i] = v;
            break;
    }
}

static inline u32 get_texel(const void *tile, int format, int i) {
    switch (gfx_texel_bytes(format)) {
        case 4:
            return ((const u32 *) tile)[i];
        case 2:
            return ((const u16 *) tile)[i];
        default:
            return ((const u8 *) tile)[i];
    }
}

/*
 * Mipmaps (roadmap item G1). Without them, a distant track texture is sampled a few texels apart from one pixel to
 * the next, so it shimmers as the kart moves. Each texture gfx_wants_mipmaps picks (bilinear ones that are opaque,
 * or repeat on both axes) gets a chain of smaller copies down to 8 texels on its short side (the GPU's smallest), box
 * filtered on the CPU when it is uploaded, which costs a third more texture memory. The GPU then picks the level,
 * blending between two (trilinear). The Settings menu switches between using the levels and level 0 only, from the
 * next frame; the levels are built either way so that switching shows the difference at once. DKR_MIPMAP=0 never
 * builds them; DKR_MIP_FILTER=0 takes the nearest level instead of blending two.
 */
static int mipmaps_built(void) {
    static int sBuild = -1;
    if (sBuild < 0) {
        const char *env = plat_getenv("DKR_MIPMAP");
        sBuild = env == NULL || env[0] != '0';
        plat_log("mipmaps: %s", sBuild ? "built for bilinear textures" : "never built");
    }
    return sBuild;
}

/* How many levels later textures clamped both ways (signs) switch to smaller ones: see gfx_mip_lod_bias.
 * DKR_MIP_SIGN_BIAS changes it (0 treats them like the rest, as before; -1 is the default). */
static float sign_bias(void) {
    static int sRead;
    static float sBias = -1.0f;
    if (!sRead) {
        const char *env = plat_getenv("DKR_MIP_SIGN_BIAS");
        sRead = 1;
        if (env != NULL) {
            sBias = (float) atof(env);
        }
        plat_log("mipmaps: signs %.2f levels sharper", -sBias);
    }
    return sBias;
}

static GPU_TEXTURE_FILTER_PARAM mip_filter(void) {
    static int sFilter = -1;
    if (sFilter < 0) {
        const char *env = plat_getenv("DKR_MIP_FILTER");
        sFilter = (env != NULL && env[0] == '0') ? GPU_NEAREST : GPU_LINEAR;
    }
    return (GPU_TEXTURE_FILTER_PARAM) sFilter;
}

/* 0 off, 1 on, 2 on with each level tinted (see tint_level), for seeing where the GPU uses which. */
void plat_3ds_set_mipmap_setting(int mode) {
    sMipmapSetting = mode < 0 ? 0 : mode > 2 ? 2 : mode;
    settings_set("mipmaps", sMipmapSetting);
}

int plat_3ds_mipmap_setting(void) {
    return sMipmapSetting;
}

/* The number of levels after the first to give a texture of the GPU size pw x ph. */
static int mip_levels(const GfxTexture *tex, unsigned int pw, unsigned int ph) {
    if (!mipmaps_built() || !gfx_wants_mipmaps(tex)) {
        return 0;
    }
    return C3D_TexCalcMaxLevel(pw, ph);
}

/* Sets the sampler: point or bilinear, with the mipmap levels in use or not, and their bias. */
static void set_texture_filter(C3DTexture *t, const GfxTexture *tex, int point) {
    if (point) {
        C3D_TexSetFilter(&t->tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetFilterMipmap(&t->tex, GPU_NEAREST);
    } else {
        C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetFilterMipmap(&t->tex, mip_filter());
    }
    t->point = point;
    t->tex.maxLevel = (sMipmapsUsed && !point) ? t->levels : 0;
    C3D_TexSetLodBias(&t->tex, t->levels > 0 ? gfx_mip_lod_bias(tex, sign_bias()) : 0.0f);
}

/* Swizzles an RGBA8888 image (w x h, both multiples of 8) into the GPU's 8x8 tiles, top row first. */
static void swizzle_level(u8 *data, const unsigned int *pixels, unsigned int w, unsigned int h, int format) {
    unsigned int bytes = gfx_texel_bytes(format), tx, ty;
    int i;
    for (ty = 0; ty < h; ty += 8) {
        for (tx = 0; tx < w; tx += 8) {
            u8 *tile = &data[((ty / 8) * (w / 8) + tx / 8) * 64 * bytes];
            for (i = 0; i < 64; i++) {
                u32 c = pixels[(ty + (i >> 3)) * w + tx + (i & 7)];
                put_texel(tile, format, sTileOrder[i], gfx_texel_encode(c, format));
            }
        }
    }
}

/*
 * The "Show" mipmap setting: the levels are tinted halfway towards red (level 1), green (2), blue (3), then yellow,
 * so the picture shows where each level is used. A texture whose levels don't match the setting has them built
 * again when it is next drawn (bind_texture), so switching takes effect at once, with a one-off hitch.
 */
static void tint_level(unsigned int *pixels, unsigned int count, int level) {
    static const unsigned int sTints[4] = { 0x0000FF, 0x00FF00, 0xFF0000, 0x00FFFF };
    unsigned int tint = sTints[(level - 1) & 3], i, k;
    for (i = 0; i < count; i++) {
        unsigned int c = pixels[i], out = c & 0xFF000000u;
        for (k = 0; k < 3; k++) {
            out |= ((((c >> (8 * k)) & 0xFF) + ((tint >> (8 * k)) & 0xFF)) / 2) << (8 * k);
        }
        pixels[i] = out;
    }
}

/* Builds and uploads the levels after the first, from the texture at GPU size. Returns 0 if out of memory. */
PLAT_NOINLINE static int upload_mip_levels(C3DTexture *t, const GfxTexture *tex, int format) {
    unsigned int pw = t->tex.width, ph = t->tex.height, x, y;
    unsigned int *pixels = malloc(pw * ph * sizeof(unsigned int));
    int level;
    if (pixels == NULL) {
        return 0;
    }
    for (y = 0; y < ph; y++) {
        for (x = 0; x < pw; x++) {
            pixels[y * pw + x] = pot_texel(tex, pw, ph, t->resample, x, y);
        }
    }
    t->tinted = sMipmapsShown;
    for (level = 1; level <= t->levels; level++) {
        gfx_mip_downsample(pixels, pw, ph, pixels);
        pw /= 2;
        ph /= 2;
        if (t->tinted) {
            // Tints the copy the next level is made from too, which only makes the tints of later levels stronger.
            tint_level(pixels, pw * ph, level);
        }
        swizzle_level(C3D_Tex2DGetImagePtr(&t->tex, level, NULL), pixels, pw, ph, format);
    }
    free(pixels);
    return 1;
}

void gfxb_texture_upload(GfxTexture *tex) {
    C3DTexture *t = (C3DTexture *) tex->backendData;
    unsigned int w = tex->width, h = tex->height;
    unsigned int pw = next_pot(w), ph = next_pot(h);
    int resample = (tex->wrapS != GFX_WRAP_CLAMP && pw != w) || (tex->wrapT != GFX_WRAP_CLAMP && ph != h);
    int format = gpu_format(tex);
    int levels = mip_levels(tex, pw, ph);
    unsigned int bytes = gfx_texel_bytes(format);
    u8 *data;
    unsigned int tx, ty;
    int i;

    if (t != NULL && (t->tex.fmt != gpu_texcolor(format) || t->levels != levels)) {
        gfxb_texture_free(tex); // Uploaded again in another format or with other levels: start afresh.
        t = NULL;
    }
    if (t == NULL) {
        t = calloc(1, sizeof(C3DTexture));
        if (t != NULL && levels > 0 &&
            !C3D_TexInitWithParams(&t->tex, NULL,
                                   (C3D_TexInitParams) { pw, ph, levels, gpu_texcolor(format), GPU_TEX_2D, false })) {
            levels = 0; // No room for the levels: try one.
        }
        if (t == NULL || (levels == 0 && !C3D_TexInit(&t->tex, pw, ph, gpu_texcolor(format)))) {
            static int sFailures;
            gPlatCounters[PLAT_COUNTER_TEXTURE_FAILED]++;
            if ((sFailures++ % 50) == 0) {
                plat_log("texture %ux%u allocation failed (%d failures, linear free %lu)", pw, ph, sFailures,
                         (unsigned long) linearSpaceFree());
            }
            free(t);
            return;
        }
        t->levels = levels;
        texture_link(t);
        tex->backendData = t;
    } else if (in_vram(t->tex.data)) {
        // Written again, by the CPU, which can't write VRAM: into new linear memory (P17).
        u32 total = C3D_TexCalcTotalSize(t->tex.size, t->levels);
        void *data = linearAlloc(total);
        C3DTexture *old = calloc(1, sizeof(C3DTexture));
        if (data == NULL || old == NULL) {
            linearFree(data);
            free(old);
            return; // Keeps the old picture.
        }
        old->tex.data = t->tex.data;
        move_shares(t->tex.data, data);
        t->tex.data = data;
        sVramTextureBytes -= total;
        defer_delete(old);
    }
    // Swizzled straight into the texture's memory in 8x8 tiles, top row first (the GPU samples t = 0 from the start
    // of the data). No temporary buffer: when the heap was short, skipping the upload left whatever the memory held
    // before, such as an old solid black texture.
    data = (u8 *) t->tex.data;
    for (ty = 0; ty < ph; ty += 8) {
        for (tx = 0; tx < pw; tx += 8) {
            u8 *tile = &data[((ty / 8) * (pw / 8) + tx / 8) * 64 * bytes];
            for (i = 0; i < 64; i++) {
                put_texel(tile, format, sTileOrder[i], gpu_texel(tex, format, pw, ph, resample, tx + (i & 7),
                                                                  ty + (i >> 3)));
            }
        }
    }
    t->resample = resample;
    t->scaleU = resample ? 1.0f : (float) w / pw;
    t->scaleV = resample ? 1.0f : (float) h / ph;
    if (t->levels > 0 && !upload_mip_levels(t, tex, format)) {
        t->levels = 0; // Out of memory for the temporary image: the levels hold nothing, so they aren't used.
    }
    set_texture_filter(t, tex, tex->filter == GFX_FILTER_POINT);
    C3D_TexSetWrap(&t->tex, wrap_param(tex->wrapS), wrap_param(tex->wrapT));
    gpu_flush(data, C3D_TexCalcTotalSize(t->tex.size, t->levels));
    if (!t->toVram && plat_switch("DKR_TEX_VRAM", 0)) {
        t->toVram = 1; // Copied over by promote_textures.
        sToVram++;
    }
    forget_gpu_state();
}

/* For snapshots: logs a texture whose memory no longer holds what was uploaded. */
PLAT_COLD static void verify_texture(const GfxTexture *tex) {
    const C3DTexture *t = (const C3DTexture *) tex->backendData;
    unsigned int pw, ph, tx, ty, wrong = 0, firstAt = 0;
    u32 firstGot = 0, firstWant = 0;
    const u8 *data;
    int format, i;

    if (t == NULL) {
        plat_log("  texture %p %dx%d has no GPU copy", (const void *) tex, tex->width, tex->height);
        return;
    }
    pw = t->tex.width;
    ph = t->tex.height;
    format = gpu_format(tex);
    data = (const u8 *) t->tex.data;
    // Read what is in memory, not what the CPU cache holds.
    GSPGPU_FlushDataCache(data, t->tex.size);
    GSPGPU_InvalidateDataCache(data, t->tex.size);
    for (ty = 0; ty < ph; ty += 8) {
        for (tx = 0; tx < pw; tx += 8) {
            const u8 *tile = &data[((ty / 8) * (pw / 8) + tx / 8) * 64 * gfx_texel_bytes(format)];
            for (i = 0; i < 64; i++) {
                u32 want = gpu_texel(tex, format, pw, ph, t->resample, tx + (i & 7), ty + (i >> 3));
                u32 got = get_texel(tile, format, sTileOrder[i]);
                if (got != want) {
                    if (wrong++ == 0) {
                        firstAt = (ty + (i >> 3)) * pw + tx + (i & 7);
                        firstGot = got;
                        firstWant = want;
                    }
                }
            }
        }
    }
    if (wrong > 0) {
        plat_log("  texture %p %dx%d at %p: %u of %u texels changed, first at %u: %08lX instead of %08lX",
                 (const void *) tex, tex->width, tex->height, (const void *) data, wrong, pw * ph, firstAt,
                 (unsigned long) firstGot, (unsigned long) firstWant);
    }
}

void gfxb_texture_free(GfxTexture *tex) {
    C3DTexture *t = (C3DTexture *) tex->backendData;
    if (t == NULL) {
        return;
    }
    tex->backendData = NULL;
    defer_delete(t);
}

/* The GPU may still be drawing with the texture, and this frame may have used it: deletes it once the GPU has drawn
 * this frame (process_pending_frees). */
static void defer_delete(C3DTexture *t) {
    FrameSlot *slot = &sSlots[sRec];
    if (slot->numFrees == slot->freesCapacity) {
        int capacity = slot->freesCapacity ? slot->freesCapacity * 2 : 256;
        C3DTexture **list = realloc(slot->frees, capacity * sizeof(C3DTexture *));
        if (list == NULL) {
            texture_unlink(t);
            if (!t->shared) {
                C3D_TexDelete(&t->tex); // Out of memory: delete now and risk a glitch.
            }
            free(t);
            return;
        }
        slot->frees = list;
        slot->freesCapacity = capacity;
    }
    slot->frees[slot->numFrees++] = t;
}

void gfxb_texture_share(GfxTexture *dst, const GfxTexture *src) {
    const C3DTexture *s = (const C3DTexture *) src->backendData;
    C3DTexture *t;
    if (s == NULL || (t = malloc(sizeof(C3DTexture))) == NULL) {
        return; // Drawn white, like a texture that couldn't be uploaded.
    }
    // The same memory with another sampler setting: only the C3D_Tex's parameters differ.
    *t = *s;
    t->shared = 1;
    t->toVram = 0;
    texture_link(t); // Follows its memory when that moves (move_shares).
    set_texture_filter(t, dst, dst->filter == GFX_FILTER_POINT);
    dst->backendData = t;
}

/******************************** Combiner ********************************/

/* What a stage's constant colour holds. */
typedef struct ConstSlot {
    int rgb;   // GFX_CC_* source in the RGB channels, or -1 if unused
    int alpha; // GFX_CC_* source in the alpha channel, or -1 if unused
} ConstSlot;

/* A ConstSlot.rgb that means the stage's constant is TevStage.rawConst. */
#define CONST_RAW (-3)

typedef struct TevStage {
    GPU_TEVSRC rgbSrc[3], alphaSrc[3];
    int rgbOp[3], alphaOp[3];
    GPU_COMBINEFUNC rgbFunc, alphaFunc;
    ConstSlot konst;
    u32 rawConst;
} TevStage;

typedef struct TevProgram {
    TevStage stages[6];
    int numStages;
    int bufferMask; // Stages whose RGB result goes to the combiner buffer (bit 0 = stage 0)
} TevProgram;

static int is_const_input(int in) {
    return in == GFX_CC_PRIM || in == GFX_CC_ENV || in == GFX_CC_PRIM_ALPHA || in == GFX_CC_ENV_ALPHA ||
           in == GFX_CC_ONE || in == GFX_CC_PRIM_LOD_FRACTION || in == GFX_CC_ZERO;
}

/* Reserves a place in the stage's constant for "in". Returns 0 if the constant is already used for something else. */
static int claim_const(ConstSlot *k, int in, int isAlphaChannel) {
    int want;
    switch (in) {
        case GFX_CC_PRIM_ALPHA:
        case GFX_CC_ENV_ALPHA:
            // Read through the alpha channel of the constant.
            want = (in == GFX_CC_PRIM_ALPHA) ? GFX_CC_PRIM : GFX_CC_ENV;
            if (k->alpha == -1 || k->alpha == want) {
                k->alpha = want;
                return 1;
            }
            return 0;
        case GFX_CC_ONE:
        case GFX_CC_ZERO:
        case GFX_CC_PRIM_LOD_FRACTION:
        default:
            if (isAlphaChannel) {
                if (k->alpha == -1 || k->alpha == in) {
                    k->alpha = in;
                    return 1;
                }
                return 0;
            }
            if (k->rgb == -1 || k->rgb == in) {
                k->rgb = in;
                return 1;
            }
            return 0;
    }
}

/* Maps a combiner input to a TEV source and operand. "prev" is what COMBINED refers to in this stage. */
static void map_input(int in, int isAlpha, GPU_TEVSRC prev, GPU_TEVSRC *src, int *op) {
    int alphaOperand = isAlpha ? GPU_TEVOP_A_SRC_ALPHA : GPU_TEVOP_RGB_SRC_ALPHA;
    int colorOperand = isAlpha ? GPU_TEVOP_A_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR;
    switch (in) {
        case GFX_CC_TEXEL0: *src = GPU_TEXTURE0; *op = colorOperand; break;
        case GFX_CC_TEXEL1: *src = GPU_TEXTURE1; *op = colorOperand; break;
        case GFX_CC_TEXEL0_ALPHA: *src = GPU_TEXTURE0; *op = alphaOperand; break;
        case GFX_CC_TEXEL1_ALPHA: *src = GPU_TEXTURE1; *op = alphaOperand; break;
        case GFX_CC_SHADE: *src = GPU_PRIMARY_COLOR; *op = colorOperand; break;
        case GFX_CC_SHADE_ALPHA: *src = GPU_PRIMARY_COLOR; *op = alphaOperand; break;
        case GFX_CC_COMBINED: *src = prev; *op = colorOperand; break;
        case GFX_CC_COMBINED_ALPHA: *src = prev; *op = alphaOperand; break;
        case GFX_CC_PRIM_ALPHA:
        case GFX_CC_ENV_ALPHA: *src = GPU_CONSTANT; *op = alphaOperand; break;
        default: *src = GPU_CONSTANT; *op = colorOperand; break; // PRIM, ENV, ONE, ZERO, LOD
    }
}

/* Compiles one (a - b) * c + d equation into a stage's RGB or alpha half. Returns 0 if constants conflict. */
static int compile_equation(TevStage *st, const GfxCCEquation *eq, int isAlpha, GPU_TEVSRC prev) {
    GPU_TEVSRC *src = isAlpha ? st->alphaSrc : st->rgbSrc;
    int *op = isAlpha ? st->alphaOp : st->rgbOp;
    GPU_COMBINEFUNC *func = isAlpha ? &st->alphaFunc : &st->rgbFunc;
    int a = eq->a, b = eq->b, c = eq->c, d = eq->d;
    int inputs[3];
    int n, i;

    if (c == GFX_CC_ZERO || a == b) {
        // Just d.
        *func = GPU_REPLACE;
        inputs[0] = d;
        n = 1;
    } else if (b == GFX_CC_ZERO && d == GFX_CC_ZERO) {
        // Checked before the interpolation below, which would otherwise need a constant for the zero.
        if (c == GFX_CC_ONE) {
            *func = GPU_REPLACE;
            inputs[0] = a;
            n = 1;
        } else {
            *func = GPU_MODULATE;
            inputs[0] = a;
            inputs[1] = c;
            n = 2;
        }
    } else if (b == d) {
        // a * c + b * (1 - c)
        *func = GPU_INTERPOLATE;
        inputs[0] = a;
        inputs[1] = b;
        inputs[2] = c;
        n = 3;
    } else if (b == GFX_CC_ZERO) {
        *func = GPU_MULTIPLY_ADD;
        inputs[0] = a;
        inputs[1] = c;
        inputs[2] = d;
        n = 3;
    } else if (a == GFX_CC_ONE && b != GFX_CC_ZERO && d == b) {
        *func = GPU_INTERPOLATE;
        inputs[0] = a;
        inputs[1] = b;
        inputs[2] = c;
        n = 3;
    } else {
        // (a - b) * c + d is approximated as a * c + d. None of the combiners DKR uses get here.
        *func = GPU_MULTIPLY_ADD;
        inputs[0] = a;
        inputs[1] = c;
        inputs[2] = d;
        n = 3;
    }
    for (i = 0; i < n; i++) {
        if (is_const_input(inputs[i]) && !claim_const(&st->konst, inputs[i], isAlpha)) {
            return 0;
        }
        map_input(inputs[i], isAlpha, prev, &src[i], &op[i]);
    }
    for (; i < 3; i++) {
        src[i] = GPU_PRIMARY_COLOR;
        op[i] = 0;
    }
    return 1;
}

static void init_stage(TevStage *st) {
    int i;
    memset(st, 0, sizeof(*st));
    st->konst.rgb = -1;
    st->konst.alpha = -1;
    for (i = 0; i < 3; i++) {
        st->rgbSrc[i] = GPU_PREVIOUS;
        st->alphaSrc[i] = GPU_PREVIOUS;
    }
    st->rgbFunc = GPU_REPLACE;
    st->alphaFunc = GPU_REPLACE;
}

/*
 * Fog. The N64 computes a fog factor per vertex from z / w. Interpolating that is wrong once the GPU clips a triangle
 * at the near plane (the N64 recomputes it for the new vertices), which washed out geometry close to the camera.
 * So by default the GPU's fog unit applies the same linear function per pixel, from a lookup table indexed by depth.
 * DKR_VERTEX_FOG=1 restores the per vertex version, DKR_NO_FOG=1 turns fog off.
 */
enum { FOG_PIXEL, FOG_VERTEX, FOG_OFF };

static int sFogMode = -1;

static void __attribute__((noinline, cold)) read_fog_mode(void) {
    sFogMode = plat_getenv("DKR_NO_FOG") != NULL ? FOG_OFF : plat_getenv("DKR_VERTEX_FOG") != NULL ? FOG_VERTEX : FOG_PIXEL;
}

static inline int fog_mode(void) {
    if (__builtin_expect(sFogMode < 0, 0)) {
        read_fog_mode();
    }
    return sFogMode;
}

static int sLutMul = 0x7FFFFFFF, sLutOffset; // What the fog table was made for
static int sFogOn = -1;

/* Builds and binds the fog table for the draw's fog settings. Out of line: a draw's fog settings mostly match the
 * table's, so the per-draw code doesn't carry this. */
static void __attribute__((noinline)) bind_fog_lut(const GfxDrawState *state) {
    static C3D_FogLut sLut;
    float data[256];
    int i;
    // Depth d runs from 1 at the near plane to 0 at the far plane, and z / w = 1 - 2d (see to_screen).
    for (i = 0; i <= 128; i++) {
        float zw = 1.0f - 2.0f * (i / 128.0f);
        float fog = zw * state->fogMul + state->fogOffset;
        float keep;
        fog = (fog < 0.0f) ? 0.0f : (fog > 255.0f) ? 255.0f : fog;
        keep = 1.0f - fog / 255.0f; // The table holds how much of the original colour remains.
        if (i < 128) {
            data[i] = keep;
        }
        if (i > 0) {
            data[i + 127] = keep - data[i - 1];
        }
    }
    FogLut_FromArray(&sLut, data);
    C3D_FogLutBind(&sLut);
    sLutMul = state->fogMul;
    sLutOffset = state->fogOffset;
}

PLAT_HOT static void apply_fog(const GfxDrawState *state) {
    int on = state->fogEnabled && fog_mode() == FOG_PIXEL;

    if (on && (state->fogMul != sLutMul || state->fogOffset != sLutOffset)) {
        bind_fog_lut(state);
    }
    if (on) {
        u32 color = state->fog[0] | (state->fog[1] << 8) | (state->fog[2] << 16);
        if (!sGpu.known || color != sGpu.fogColor) {
            C3D_FogColor(color);
            sGpu.fogColor = color;
        }
    }
    if (!sGpu.known || on != sFogOn) {
        C3D_FogGasMode(on ? GPU_FOG : GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        sFogOn = on;
    }
}

/*
 * Colour boost (roadmap item G2). The 3DS screen looks washed out next to a CRT, so the colour of every draw can be
 * pushed away from its grey: out = c + t * (c - L), where L is c's luma (0.299 R + 0.587 G + 0.114 B) and t the
 * strength. That keeps black, white and greys as they were, and as it is linear, blending and fog come out as if it
 * were applied to the finished picture. The combiner has six stages and DKR's combiners use one or two; four more
 * work it out, each stage's result clamped to 0..1, so every step is arranged to stay in range:
 *  - the game's last stage also writes its colour c to the combiner buffer,
 *  - y = K c + A                                      (K = t / (2 (1 - t)), A = 1/2 - 1/(4 (1 - t)))
 *  - f = dot3(1 - y, w / 2 + 1/2) = 1/2 + ... = (1/2 - t L) / (1 - t)
 *  - D = t c + (1 - t) f = t (c - L) + 1/2            (the buffer's c, then f)
 *  - out = c + D - 1/2                                (signed add)
 * dot3 computes 4 * sum((a - 1/2) (b - 1/2)), which is where the halves go. t is at most 1/2 so that A stays above
 * zero. The alpha is passed through untouched (or worked out by a stage the game's combiner needed for alpha only,
 * see alpha_only_stage). A combiner that still needs more than two stages is drawn without it: the buffer can only
 * be written by the first four stages. (The first version missed that DKR's track combiner takes three, so the boost
 * left most of the picture as it was.)
 */
static const float sBoostStrength[4] = { 0.0f, 0.15f, 0.3f, 0.5f };

static u8 unit_byte(float v) {
    return (u8) (v * 255.0f + 0.5f);
}

static void boost_stage(TevStage *st, GPU_COMBINEFUNC func, GPU_TEVSRC a, int aOp, GPU_TEVSRC b, int bOp, GPU_TEVSRC c,
                        int cOp, u32 konst) {
    init_stage(st);
    st->rgbFunc = func;
    st->rgbSrc[0] = a;
    st->rgbSrc[1] = b;
    st->rgbSrc[2] = c;
    st->rgbOp[0] = aOp;
    st->rgbOp[1] = bOp;
    st->rgbOp[2] = cOp;
    st->konst.rgb = CONST_RAW;
    st->rawConst = konst;
}

/*
 * Whether a stage only works on alpha and passes the colour through: the second half of a cycle whose colour and
 * alpha equations needed different constants (compile_combiner). Its alpha half can ride along in a colour boost
 * stage, whose alpha half is free.
 */
static int alpha_only_stage(const TevStage *st) {
    int i;
    if (st->rgbFunc != GPU_REPLACE || st->rgbSrc[0] != GPU_PREVIOUS || st->rgbOp[0] != GPU_TEVOP_RGB_SRC_COLOR ||
        st->konst.rgb != -1) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        if (st->alphaSrc[i] == GPU_CONSTANT && st->konst.alpha == -1) {
            return 0; // Reads the zero constant of a first cycle: keep it where it is.
        }
    }
    return 1;
}

static void add_colour_boost(TevProgram *prog, int level) {
    float t = sBoostStrength[level];
    u8 k = unit_byte(t / (2.0f * (1.0f - t))), a = unit_byte(0.5f - 0.25f / (1.0f - t)), lerp = unit_byte(t);
    u32 weights = unit_byte(0.5f + 0.299f / 2) | (unit_byte(0.5f + 0.587f / 2) << 8) |
                  (unit_byte(0.5f + 0.114f / 2) << 16);
    TevStage *st, alphaStage;
    int moveAlpha;

    if (level <= 0 || prog->numStages < 1) {
        return;
    }
    // DKR's most common combiner (track geometry: texture times shade, then a blend towards the environment colour
    // with the primitive alpha) takes three stages, the last for alpha only. Its alpha moves into the dot3 stage.
    moveAlpha = prog->numStages == 3 && alpha_only_stage(&prog->stages[2]);
    if (moveAlpha) {
        alphaStage = prog->stages[2];
        prog->numStages = 2;
    }
    if (prog->numStages > 2) {
        static int sLogged;
        if (!sLogged) {
            sLogged = 1;
            plat_log("colour boost: a combiner of %d stages is drawn without it", prog->numStages);
        }
        return;
    }
    prog->bufferMask = 1 << (prog->numStages - 1);
    st = &prog->stages[prog->numStages++];
    boost_stage(st, GPU_MULTIPLY_ADD, GPU_PREVIOUS, GPU_TEVOP_RGB_SRC_COLOR, GPU_CONSTANT, GPU_TEVOP_RGB_SRC_COLOR,
                GPU_CONSTANT, GPU_TEVOP_RGB_SRC_ALPHA, k | (k << 8) | (k << 16) | ((u32) a << 24));
    st = &prog->stages[prog->numStages++];
    boost_stage(st, GPU_DOT3_RGB, GPU_PREVIOUS, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_CONSTANT,
                GPU_TEVOP_RGB_SRC_COLOR, GPU_PREVIOUS, GPU_TEVOP_RGB_SRC_COLOR, weights);
    if (moveAlpha) {
        memcpy(st->alphaSrc, alphaStage.alphaSrc, sizeof(st->alphaSrc));
        memcpy(st->alphaOp, alphaStage.alphaOp, sizeof(st->alphaOp));
        st->alphaFunc = alphaStage.alphaFunc;
        st->konst.alpha = alphaStage.konst.alpha;
    }
    st = &prog->stages[prog->numStages++];
    boost_stage(st, GPU_INTERPOLATE, GPU_PREVIOUS_BUFFER, GPU_TEVOP_RGB_SRC_COLOR, GPU_PREVIOUS,
                GPU_TEVOP_RGB_SRC_COLOR, GPU_CONSTANT, GPU_TEVOP_RGB_SRC_COLOR, lerp | (lerp << 8) | (lerp << 16));
    st = &prog->stages[prog->numStages++];
    boost_stage(st, GPU_ADD_SIGNED, GPU_PREVIOUS_BUFFER, GPU_TEVOP_RGB_SRC_COLOR, GPU_PREVIOUS,
                GPU_TEVOP_RGB_SRC_COLOR, GPU_PREVIOUS, GPU_TEVOP_RGB_SRC_COLOR, 0);
}

/* Out of line: only a draw whose combiner differs from the last one compiled runs it, so the per-draw code doesn't
 * carry it. */
static void __attribute__((noinline)) compile_combiner(const GfxDrawState *state, TevProgram *prog, int boost) {
    static const GfxCCEquation passthrough = { GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_COMBINED };
    int cycle;

    prog->numStages = 0;
    prog->bufferMask = 0;
    for (cycle = 0; cycle < state->combiner.numCycles; cycle++) {
        TevStage *st = &prog->stages[prog->numStages];
        // COMBINED reads the previous stage. In the first cycle there is none, so it reads zero (a constant).
        GPU_TEVSRC prev = (prog->numStages == 0) ? GPU_CONSTANT : GPU_PREVIOUS;

        init_stage(st);
        if (compile_equation(st, &state->combiner.rgb[cycle], 0, prev) &&
            compile_equation(st, &state->combiner.alpha[cycle], 1, prev)) {
            prog->numStages++;
            continue;
        }
        // The colour and alpha equations need different constants: split them over two stages.
        init_stage(st);
        compile_equation(st, &state->combiner.rgb[cycle], 0, prev);
        compile_equation(st, &passthrough, 1, prev);
        prog->numStages++;
        st = &prog->stages[prog->numStages];
        init_stage(st);
        compile_equation(st, &passthrough, 0, GPU_PREVIOUS);
        compile_equation(st, &state->combiner.alpha[cycle], 1, prev == GPU_CONSTANT ? GPU_CONSTANT : GPU_PREVIOUS);
        prog->numStages++;
    }
    if (state->fogEnabled && fog_mode() == FOG_VERTEX) {
        // Blend towards the fog colour by the vertex alpha, keeping the combined alpha.
        TevStage *st = &prog->stages[prog->numStages++];
        init_stage(st);
        st->rgbFunc = GPU_INTERPOLATE;
        st->rgbSrc[0] = GPU_CONSTANT;
        st->rgbSrc[1] = GPU_PREVIOUS;
        st->rgbSrc[2] = GPU_PRIMARY_COLOR;
        st->rgbOp[0] = GPU_TEVOP_RGB_SRC_COLOR;
        st->rgbOp[1] = GPU_TEVOP_RGB_SRC_COLOR;
        st->rgbOp[2] = GPU_TEVOP_RGB_SRC_ALPHA;
        st->konst.rgb = -2; // fog colour
    }
    add_colour_boost(prog, boost);
}

static u32 const_value(const GfxDrawState *state, int source, int channel) {
    const unsigned char *c;
    switch (source) {
        case GFX_CC_PRIM: c = state->prim; break;
        case GFX_CC_ENV: c = state->env; break;
        case -2: c = state->fog; break;
        case GFX_CC_ONE: return 255;
        case GFX_CC_PRIM_LOD_FRACTION: return state->primLodFrac;
        default: return 0;
    }
    return c[channel];
}

/* What the combiner stages are made from. */
typedef struct CombinerInputs {
    GfxCombiner combiner;
    unsigned char prim[4], env[4], fog[4];
    unsigned char primLodFrac;
    unsigned char fogStage;
    unsigned char boost;
} __attribute__((aligned(4))) CombinerInputs; // Whole words, compared as such (apply_combiner)

/* Whether two blocks of n words are the same: in place of a library memcmp, a call into code that is otherwise never
 * in the instruction cache (what the per-draw code pays for it). */
static inline int same_words(const void *a, const void *b, unsigned int n) {
    const u32 *wa = (const u32 *) a, *wb = (const u32 *) b;
    unsigned int i;
#pragma GCC unroll 1
    for (i = 0; i < n; i++) {
        if (wa[i] != wb[i]) {
            return 0;
        }
    }
    return 1;
}

PLAT_HOT static void apply_combiner(const GfxDrawState *state) {
    // The last combiner compiled, which most batches share with the one before.
    static TevProgram sProg;
    static CombinerInputs sLast;
    static int sCompiled;
    CombinerInputs in;
    int i;

    memset(&in, 0, sizeof(in));
    in.combiner = state->combiner;
    memcpy(in.prim, state->prim, 4);
    memcpy(in.env, state->env, 4);
    memcpy(in.fog, state->fog, 4);
    in.primLodFrac = state->primLodFrac;
    in.fogStage = state->fogEnabled && fog_mode() == FOG_VERTEX;
    // Not for the depth only draws that clear the depth buffer, which write no colour.
    in.boost = sDepthOnly ? 0 : sColourBoost;
    _Static_assert(sizeof(in) % 4 == 0 && sizeof(C3D_TexEnv) % 4 == 0, "the combiner inputs and stages are compared as words");
    if (sCompiled && same_words(&in, &sLast, sizeof(in) / 4)) {
        if (sGpu.known) {
            return; // Same stages as the last batch.
        }
    } else if (!sCompiled || memcmp(&in.combiner, &sLast.combiner, sizeof(in.combiner)) != 0 ||
               in.fogStage != sLast.fogStage || in.boost != sLast.boost) {
        compile_combiner(state, &sProg, in.boost);
        sCompiled = 1;
    }
    sLast = in;
    for (i = 0; i < 6; i++) {
        C3D_TexEnv stage;
        C3D_TexEnv *env = &stage;
        C3D_TexEnvInit(env);
        if (i < sProg.numStages) {
            const TevStage *st = &sProg.stages[i];
            u32 r, g, b, a;
            C3D_TexEnvSrc(env, C3D_RGB, st->rgbSrc[0], st->rgbSrc[1], st->rgbSrc[2]);
            C3D_TexEnvSrc(env, C3D_Alpha, st->alphaSrc[0], st->alphaSrc[1], st->alphaSrc[2]);
            C3D_TexEnvOpRgb(env, st->rgbOp[0], st->rgbOp[1], st->rgbOp[2]);
            C3D_TexEnvOpAlpha(env, st->alphaOp[0], st->alphaOp[1], st->alphaOp[2]);
            C3D_TexEnvFunc(env, C3D_RGB, st->rgbFunc);
            C3D_TexEnvFunc(env, C3D_Alpha, st->alphaFunc);
            if (st->konst.rgb == CONST_RAW) {
                a = st->rawConst >> 24;
                if (st->konst.alpha != -1) {
                    a = const_value(state, st->konst.alpha, 3);
                    if (st->konst.alpha == GFX_CC_ONE || st->konst.alpha == GFX_CC_PRIM_LOD_FRACTION) {
                        a = const_value(state, st->konst.alpha, 0);
                    }
                }
                C3D_TexEnvColor(env, (st->rawConst & 0xFFFFFF) | (a << 24));
            } else {
                r = const_value(state, st->konst.rgb, 0);
                g = const_value(state, st->konst.rgb, 1);
                b = const_value(state, st->konst.rgb, 2);
                a = const_value(state, st->konst.alpha, 3);
                if (st->konst.alpha == GFX_CC_ONE || st->konst.alpha == GFX_CC_PRIM_LOD_FRACTION) {
                    a = const_value(state, st->konst.alpha, 0);
                }
                C3D_TexEnvColor(env, r | (g << 8) | (b << 16) | (a << 24));
            }
        } else {
            // Unused stages pass the previous stage through.
            C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        }
        if (!sGpu.known || !same_words(&stage, &sGpu.texEnv[i], sizeof(stage) / 4)) {
            C3D_SetTexEnv(i, &stage);
            sGpu.texEnv[i] = stage;
        }
    }
    if (!sGpu.known || sProg.bufferMask != sGpu.texEnvBuf) {
        C3D_TexEnvBufUpdate(C3D_RGB, sProg.bufferMask);
        sGpu.texEnvBuf = sProg.bufferMask;
    }
}

/******************************** Drawing ********************************/

/* The Show mipmap setting changed since a texture's levels were built: builds them again. The GPU may still be
 * drawing the last frame with them, which can show one frame of mixed levels. Off the per-draw path: cold. */
static void __attribute__((noinline, cold)) retint_levels(C3DTexture *t, GfxTexture *tex) {
    if (upload_mip_levels(t, tex, gpu_format(tex))) {
        gpu_flush(t->tex.data, C3D_TexCalcTotalSize(t->tex.size, t->levels));
    }
    t->tinted = sMipmapsShown;
}

/* Returns whether the texture changed. */
PLAT_HOT static int bind_texture(int unit, GfxTexture *tex, float *scaleU, float *scaleV) {
    C3DTexture *t = (tex != NULL) ? (C3DTexture *) tex->backendData : NULL;
    C3D_Tex *bind = (t != NULL) ? &t->tex : &sWhiteTex;
    int changed = !sGpu.known || bind != sGpu.tex[unit];
    if (t != NULL) {
        *scaleU = t->scaleU;
        *scaleV = t->scaleV;
        // Only a texture with levels has mipmap settings to keep up to date: without them its maxLevel is 0 already
        // (set_texture_filter).
        if (t->levels > 0) {
            int maxLevel;
            if (t->tinted != sMipmapsShown && tex->pixels != NULL && !in_vram(t->tex.data)) {
                retint_levels(t, tex);
            }
            maxLevel = (sMipmapsUsed && !t->point) ? t->levels : 0;
            if (t->tex.maxLevel != maxLevel) {
                t->tex.maxLevel = maxLevel; // The mipmap setting changed since it was set.
                changed = 1;
            }
        }
    } else {
        *scaleU = 1.0f;
        *scaleV = 1.0f;
    }
    if (changed) {
        C3D_TexBind(unit, bind);
        sGpu.tex[unit] = bind;
    }
    return changed;
}

/* Converts a scissor rectangle on the N64 screen to the rotated framebuffer, for a flat draw or a 3D one. Out of
 * line: it runs when the scissor changes, a few times a frame, not for every draw. */
static void __attribute__((noinline)) set_scissor(const short rect[4], int flat) {
    float x0, x1;
    int left, right, top, bottom;
    if (flat && sWide) {
        x0 = gfx_flat_px(rect[0], NARROW, TOP_WIDTH);
        x1 = gfx_flat_px(rect[2], NARROW, TOP_WIDTH);
    } else {
        float scale = (flat ? NARROW : sScale3D) * ((float) TOP_WIDTH / GFX_SCREEN_WIDTH);
        x0 = TOP_WIDTH / 2 + (rect[0] - GFX_SCREEN_WIDTH / 2) * scale;
        x1 = TOP_WIDTH / 2 + (rect[2] - GFX_SCREEN_WIDTH / 2) * scale;
        if (sWide && !flat && rect[2] >= GFX_SCREEN_WIDTH - 1) {
            x1 = TOP_WIDTH; // A scissor ending at x = 319 would leave the last columns of the wide screen undrawn.
        }
    }
    // Framebuffer x runs along the screen's y axis (bottom to top), framebuffer y along the screen's x axis.
    left = TOP_HEIGHT - rect[3];
    right = TOP_HEIGHT - rect[1];
    top = TOP_WIDTH - (int) x1;
    bottom = TOP_WIDTH - (int) x0;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > TOP_HEIGHT) right = TOP_HEIGHT;
    if (bottom > TOP_WIDTH) bottom = TOP_WIDTH;
    {
        static int sNoScissor = -1;
        if (sNoScissor < 0) {
            sNoScissor = plat_getenv("DKR_NO_SCISSOR") != NULL;
        }
        if (sNoScissor) {
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
            return;
        }
    }
    if (right <= left || bottom <= top) {
        // Nothing inside: citro3d would wrap an empty rectangle's right - 1 round to 1023, turning the scissor off on
        // that side. Keeping out everything inside the whole screen draws nothing instead.
        C3D_SetScissor(GPU_SCISSOR_INVERT, 0, 0, TOP_HEIGHT * sRenderScale, TOP_WIDTH * sScaleX);
        return;
    }
    C3D_SetScissor(GPU_SCISSOR_NORMAL, left * sRenderScale, top * sScaleX, right * sRenderScale, bottom * sScaleX);
}

/*
 * Snapshots on a real New 3DS showed small blended draws (a kart shadow, one letter of text) drawn as if their
 * texture were fully opaque, or their blending off: the shadow came out as a solid black square, the letter as a white
 * box, although the command list set exactly the same state as for the same draws that came out right. Each time the
 * draw before was a large opaque one, so the GPU looks to still be busy with it while the next draw's settings are
 * written. Flushing the framebuffer between them (DKR_FB_FLUSH=1, or 2 for every draw) did not help.
 *
 * DKR_GPU_SPLIT ends the command list before such a draw and starts a new one, which the GPU only begins once it has
 * finished the previous list: 1 before a blended draw that follows an unblended one, 2 whenever the blending, alpha
 * test or depth settings change, 0 (the default, see below) never. This fixed the letter but not the shadows, which
 * come later in a race frame than the first 12 splits allowed. The GPU's command queue holds 32 entries a frame and a
 * frame needs 3 of its own, so at most MAX_DRAIN_SPLITS are made; past SMALL_DRAWS_ONLY, only for draws of a few
 * triangles, which are the ones that showed the problem.
 */
#define MAX_DRAIN_SPLITS 24
#define SMALL_DRAWS_ONLY 16
#define IS_BLENDED(blend) ((blend) == GFX_BLEND_ALPHA || (blend) == GFX_BLEND_ADD)

/*
 * The kart shadow still came out as a black square in the hub with the split above. Testing on a New 3DS showed that
 * writing the settings of the first blended draw twice fixes it (an empty triangle before it only sometimes did): the
 * GPU seems to miss some of the settings written right after an unblended draw. Both the split and the second write
 * are done.
 *
 * With the settings written twice the splits turned out not to be needed, and they cost a lot: the switch bench
 * (issue #152, 60 fps) raced the three heaviest tracks with them off at the same average GPU and renderer times, but
 * with no frame over 17 ms instead of 33-50 ms. So since roadmap P19 they are off by default (DKR_GPU_SPLIT=0), with
 * the second write kept; DKR_GPU_SPLIT=1 puts them back.
 */

/*
 * The log behind the character select screen came and went from frame to frame on a New 3DS (issue #25). It is the
 * first draw after the screen clears, which are full screen draws with fog, depth and culling off, and the sky drawn
 * after it would cover it wherever the GPU drew it without its depth settings. So the draw after a clear rectangle is treated like
 * the first blended draw above: its settings are written twice (and with DKR_GPU_SPLIT=1 the command list is split
 * before it).
 * DKR_AFTER_CLEAR=0 leaves that out, for testing.
 */
#define KEY_CLEAR (1u << 19)

/*
 * The Rareware logo in the intro came and went on a New 3DS too. Each fault so far was a draw whose blending or depth
 * settings differ from the draw before it, and only the cases already seen were covered. Two more are left that the
 * intro has every frame: the first draw of a frame (the depth clear, after the previous frame's full screen blended
 * fade; if the GPU missed its depth settings the depth buffer keeps the last frame's logo, and the turning logo fails
 * the depth test against itself), and any other change of the blending, alpha test or depth settings (the sky after
 * the black fill, opaque logo parts after its blended decals). So every such draw now has its settings written twice.
 * Only the known cases also split the command list, as the GPU allows few splits a frame. A race frame has about 35
 * such draws out of about 155. DKR_REWRITE=0 writes twice only in the known cases, as before.
 */

static int sFlushMode = -1, sAfterClear = -1, sRewrite = -1; // drain_before_draw's settings, read on its first call
static unsigned int sLastKey = ~0u;

static void __attribute__((noinline, cold)) read_drain_settings(void) {
    const char *env = plat_getenv("DKR_FB_FLUSH");
    sFlushMode = env != NULL ? atoi(env) : 0;
    plat_log("framebuffer flush between draws: %s",
             sFlushMode == 0 ? "off" : sFlushMode == 1 ? "on state changes" : "always");
    env = plat_getenv("DKR_AFTER_CLEAR");
    sAfterClear = env != NULL ? atoi(env) : 1;
    env = plat_getenv("DKR_REWRITE");
    sRewrite = env != NULL ? atoi(env) : 1;
    plat_log("settings written twice: %s", sRewrite ? "on every blend or depth change" : "known cases only");
}

/* Returns whether the draw's settings are to be written twice: a draw whose blending or depth settings differ from
 * the draw before, or the first draw of a frame. */
PLAT_HOT static int drain_before_draw(const GfxDrawState *state, int numTriangles) {
    unsigned int key;
    int split, first;

    if (__builtin_expect(sFlushMode < 0, 0)) {
        read_drain_settings();
    }
    key = state->blend | (state->alphaTest << 4) | (state->alphaRef << 8) | (state->depthTest << 16) |
          (state->depthWrite << 17) | (sDepthOnly << 18) | (sClearing ? KEY_CLEAR : 0);
    if (sFlushMode == 2 || (sFlushMode == 1 && key != sLastKey)) {
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
        GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    }
    first = (IS_BLENDED(state->blend) && !IS_BLENDED(sLastKey & 0xF)) ||
            (sAfterClear && !sClearing && sLastKey != ~0u && (sLastKey & KEY_CLEAR));
    split = sSplitMode == 2 ? key != sLastKey : sSplitMode == 1 ? first : 0;
    if (split && sVboUsed > 0 && sDrainSplits < MAX_DRAIN_SPLITS &&
        (sDrainSplits < SMALL_DRAWS_ONLY || numTriangles <= 4)) {
        // The GPU reads the vertices so far when it runs the first part of the list.
        flush_new_vertices();
        split_command_list();
        sDrainSplits++;
    }
    if (sRewrite && (sFrameFirstDraw || key != sLastKey)) {
        first = 1;
    }
    sFrameFirstDraw = 0;
    sLastKey = key;
    return first;
}

/*
 * DKR_DIAG_ALPHA=1, for finding the cause of the boxes above: blended draws show their texture's alpha as grey and
 * are drawn half transparent. A box that comes out grey has the right blending but an opaque texture; one that shows
 * the letter or shadow shape at full brightness has the right texture but no blending.
 */
static int sDiagAlpha = -1; // Read on the first call; set_all_state only calls while it isn't known to be off

static int __attribute__((noinline, cold)) apply_alpha_diagnostic(const GfxDrawState *state) {
    int i;

    if (sDiagAlpha < 0) {
        sDiagAlpha = plat_getenv("DKR_DIAG_ALPHA") != NULL;
    }
    if (!sDiagAlpha || !IS_BLENDED(state->blend)) {
        return 0;
    }
    for (i = 0; i < 6; i++) {
        C3D_TexEnv *env = C3D_GetTexEnv(i);
        C3D_TexEnvInit(env);
        if (i == 0) {
            C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
            C3D_TexEnvColor(env, 0x80FFFFFF);
        } else {
            C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        }
    }
    return 1;
}

static int sNoCull = -1; // DKR_NO_CULL, read on set_draw_state's first call

static void __attribute__((noinline, cold)) read_cull_setting(void) {
    sNoCull = plat_getenv("DKR_NO_CULL") != NULL;
}

PLAT_HOT static void set_draw_state(const GfxDrawState *state) {
    u8 depthEnable, depthFunc, depthMask, cull, early;

    if (!sGpu.known || state->blend != sGpu.blend) {
        switch (state->blend) {
            case GFX_BLEND_ALPHA:
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
                break;
            case GFX_BLEND_ADD:
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ONE, GPU_ZERO);
                break;
            default:
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
                break;
        }
        sGpu.blend = state->blend;
    }
    if (!sGpu.known || state->alphaTest != sGpu.alphaTest || state->alphaRef != sGpu.alphaRef) {
        C3D_AlphaTest(state->alphaTest, GPU_GEQUAL, state->alphaRef);
        sGpu.alphaTest = state->alphaTest;
        sGpu.alphaRef = state->alphaRef;
    }
    if (sDepthOnly) {
        depthEnable = true;
        depthFunc = GPU_ALWAYS;
        depthMask = GPU_WRITE_DEPTH;
    } else if (state->depthTest || state->depthWrite) {
        depthEnable = true;
        depthFunc = state->depthTest ? GPU_GEQUAL : GPU_ALWAYS;
        depthMask = state->depthWrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR;
    } else {
        depthEnable = false;
        depthFunc = GPU_ALWAYS;
        depthMask = GPU_WRITE_COLOR;
    }
    if (!sGpu.known || depthEnable != sGpu.depthEnable || depthFunc != sGpu.depthFunc ||
        depthMask != sGpu.depthMask) {
        C3D_DepthTest(depthEnable, depthFunc, depthMask);
        sGpu.depthEnable = depthEnable;
        sGpu.depthFunc = depthFunc;
        sGpu.depthMask = depthMask;
    }
    // Early depth (P20) only for solid draws with the usual depth test and depth writes: it would hide what a draw
    // without the test (the sky, the HUD) puts in front, and a cutout (alpha test: fences, trees) or blended draw
    // marks its whole triangles in the early depth buffer, gaps included, which left black blocks behind the fences
    // of Jungle Falls on a New 3DS. Not in 3D, where both eyes' draws share the one early depth buffer.
    // Even so a New 3DS still lost random blocks all over the picture (stale early depth?), so it stays off by default
    // and out of the switch bench until P20 sorts it out.
    early = sEarlyDepth && !sStereo && !sDepthOnly && depthEnable && depthFunc == GPU_GEQUAL &&
            depthMask == GPU_WRITE_ALL && !state->alphaTest && !IS_BLENDED(state->blend);
    if (!sGpu.known || early != sGpu.earlyDepth) {
        C3D_EarlyDepthTest(early, GPU_EARLYDEPTH_GEQUAL, 0);
        sGpu.earlyDepth = early;
    }
    if (!sGpu.known || state->decal != sGpu.decal) {
        // Widescreen's camera cuts 4 times closer (plat_camera_near), which spreads depth 4 times as far: decals are
        // pulled a quarter as much to stay as close to their surfaces.
        C3D_DepthMap(true, -1.0f, state->decal ? 0.0002f * plat_camera_near() / 10.0f : 0.0f);
        sGpu.decal = state->decal;
    }
    if (__builtin_expect(sNoCull < 0, 0)) {
        read_cull_setting();
    }
    cull = (sNoCull || state->cullMode == 0) ? GPU_CULL_NONE
           : state->cullMode == 1             ? GPU_CULL_BACK_CCW
                                              : GPU_CULL_FRONT_CCW;
    if (!sGpu.known || cull != sGpu.cull) {
        C3D_CullFace(cull);
        sGpu.cull = cull;
    }
    if (!sGpu.known || memcmp(state->scissor, sGpu.scissor, sizeof(sGpu.scissor)) != 0 ||
        (state->flat != 0) != sGpu.scissorFlat) {
        set_scissor(state->scissor, state->flat);
        memcpy(sGpu.scissor, state->scissor, sizeof(sGpu.scissor));
        sGpu.scissorFlat = state->flat != 0;
    }
}

/*
 * With only changed settings sent, a batch that only changes its texture put the texture unit settings and the
 * texture cache clear straight after the previous draw in the command list. Twice on a real New 3DS the GPU hung in
 * a stretch of Crescent Island made of such batches (small cutout trees, 2 to 4 triangles each), which had never
 * happened in 200 races while every setting was sent for every batch. So a texture change also sends the blending,
 * depth and culling settings again, which citro3d writes before the texture ones, as it always did before.
 * DKR_STATE_CACHE=2 leaves that out, for testing.
 */

/*
 * More than half of a race frame's draws differ from the draw before only in their textures (PC count, P23). For
 * those the combiner, fog and draw settings below would only find again that nothing changed, so with DKR_FAST_STATE
 * (the default) they are skipped: the draw is compared with the last one whose settings were all checked, which is
 * kept here. The comparison leaves out the padding byte after fogEnabled. What else the settings depend on (colour
 * boost, wide screen, 3D, early depth) only changes between frames, when the settings are forgotten anyway; the depth
 * clear's sDepthOnly is compared too.
 */
static GfxDrawState sCheckedState;
static int sCheckedDepthOnly;

/* Compared a word at a time in place (the fields up to fogEnabled are the first nine words and a byte): a library
 * memcmp is a call into code that is otherwise never in the instruction cache, which is what a draw pays for here. */
static inline int same_but_textures(const GfxDrawState *a, const GfxDrawState *b) {
    const u32 *wa = (const u32 *) a, *wb = (const u32 *) b;
    int i;
    _Static_assert(offsetof(GfxDrawState, fogEnabled) == 36 && offsetof(GfxDrawState, scissor) % 4 == 0 &&
                       sizeof(a->scissor) == 8,
                   "same_but_textures compares the draw state's words");
    for (i = 0; i < 9; i++) {
        if (wa[i] != wb[i]) {
            return 0;
        }
    }
    return a->fogEnabled == b->fogEnabled && a->fogMul == b->fogMul && a->fogOffset == b->fogOffset &&
           a->cullMode == b->cullMode && a->flat == b->flat &&
           wa[offsetof(GfxDrawState, scissor) / 4] == wb[offsetof(GfxDrawState, scissor) / 4] &&
           wa[offsetof(GfxDrawState, scissor) / 4 + 1] == wb[offsetof(GfxDrawState, scissor) / 4 + 1];
}

static int sStateCache = -1; // DKR_STATE_CACHE, read on set_all_state's first call

static void __attribute__((noinline, cold)) read_state_cache_setting(void) {
    const char *env = plat_getenv("DKR_STATE_CACHE");
    sStateCache = env != NULL ? atoi(env) : 1;
    plat_log("GPU settings: %s", sStateCache == 0   ? "all set for every draw"
                                 : sStateCache == 2 ? "only changes are set"
                                                    : "only changes are set, all draw settings with a new texture");
}

/* Sets every GPU setting a draw uses. */
PLAT_HOT static void set_all_state(const GfxDrawState *state, float *su0, float *sv0, float *su1, float *sv1) {
    int newTexture;

    if (__builtin_expect(sStateCache < 0, 0)) {
        read_state_cache_setting();
    }
    if (sFastState && sGpu.known && sDepthOnly == sCheckedDepthOnly && same_but_textures(state, &sCheckedState)) {
        newTexture = bind_texture(0, state->textures[0], su0, sv0);
        newTexture |= bind_texture(1, state->textures[1], su1, sv1);
        if (newTexture && sStateCache == 1) {
            C3D_CullFace((GPU_CULLMODE) sGpu.cull); // As below
        }
        return;
    }
    newTexture = bind_texture(0, state->textures[0], su0, sv0);
    newTexture |= bind_texture(1, state->textures[1], su1, sv1);
    apply_combiner(state);
    apply_fog(state);
    set_draw_state(state);
    if (newTexture && sGpu.known && sStateCache == 1) {
        C3D_CullFace((GPU_CULLMODE) sGpu.cull); // Same value: only marks the group for writing.
    }
    sGpu.known = sStateCache != 0;
    if (__builtin_expect(sDiagAlpha != 0, 0) && apply_alpha_diagnostic(state)) {
        // It wrote the combiner stages itself.
        forget_gpu_state();
    }
    sCheckedState = *state;
    sCheckedDepthOnly = sDepthOnly;
}

/*
 * citro3d's flags for the groups of settings that set_all_state sets when nothing is known (sGpu forgotten): the
 * effect (blending, depth, culling), the scissor, the combiner buffer and fog, the texture units and the combiner
 * stages. From citro3d's internal.h (C3DiF_Effect, _Scissor, _TexEnvBuf, _TexAll, _TexEnvAll).
 */
#define C3D_SETTINGS_FLAGS ((1u << 4) | (1u << 7) | (1u << 9) | (7u << 23) | (0x3Fu << 26))

/*
 * Writes all of a draw's settings now, and leaves them all marked for the draw to write a second time
 * (drain_before_draw says why). The settings were all just set with nothing known. The full way sets them all again
 * after the first write; the short way (DKR_FAST_STATE) puts back the flags the first write cleared, which marks the
 * same groups with the same values, so the command list comes out the same for less work.
 */
PLAT_HOT static void write_settings_twice(const GfxDrawState *state, float *su0, float *sv0, float *su1, float *sv1) {
    if (sFastState) {
        u32 again = __C3D_Context.flags & C3D_SETTINGS_FLAGS;
        C3Di_UpdateContext();
        __C3D_Context.flags |= again;
        return;
    }
    C3Di_UpdateContext();
    forget_gpu_state();
    set_all_state(state, su0, sv0, su1, sv1);
}

/* Triangles reaching far off the screen, such as the sky dome and big stretches of ground around the camera, are cut
 * down to a guard band before they reach the GPU, as the N64 does. The 3DS's GPU can drop such triangles or draw them
 * with stray lines of pixels. Its rasterizer seems to take corners only up to about 1024 pixels from the corner of the
 * render target: a band twice the screen's size put corners at 1200 pixels on an 800-pixel target, and those
 * triangles vanished (the hub's grass, #147). So the band stays within GUARD_PIXELS of the target's corner on each
 * axis, and at most twice the screen. Flat draws are cut before gfx_flat_x narrows them, which moves the texture a
 * little only on what crosses the band. Settings > Screen > Edge trimming Off (or DKR_GUARD_BAND=0) = no cutting. */
#define GUARD_BAND 2.0f
#define GUARD_PIXELS 1000.0f

static volatile int sGuardBand = -1; // Settings > Screen > Edge trimming (settings.txt "guardband"), else DKR_GUARD_BAND
static float sBandX = GUARD_BAND, sBandY = GUARD_BAND; // The band in clip space: |x| <= sBandX * w, |y| <= sBandY * w
static GfxVertex *sClipVerts;
static u16 *sClipIndices;
static u8 *sClipOutside; // For each vertex of the draw: outside the guard band
static int sClipCapacity, sClipIndexCapacity, sClipOutsideCapacity;

/* The band for an axis of the render target that many pixels long: its far edge at GUARD_PIXELS, at most GUARD_BAND. */
static float band_for(int pixels) {
    float band = 2.0f * GUARD_PIXELS / pixels - 1.0f;
    return band < GUARD_BAND ? band : GUARD_BAND;
}

/* The band for the frame's render target, once a frame for the lean draw (the full draw works it out for each draw,
 * as it always did; the GPU self test draws before the first frame). Clip x runs along the target's height (the
 * screen's 400 or 800 pixels across), clip y along its width. */
static void set_frame_band(void) {
    sBandX = band_for(sTarget->frameBuf.height);
    sBandY = band_for(sTarget->frameBuf.width);
}

/* Which sides of the band a vertex is beyond: bit p for clip_distance's plane p, OUTSIDE_BEHIND for w <= 0. */
#define OUTSIDE_PLANES 0xF
#define OUTSIDE_BEHIND 0x10
static inline int outside_band_of(const GfxVertex *v, float bandX, float bandY) {
    float gx = bandX * v->w, gy = bandY * v->w;
    return (v->x > gx) | ((-v->x > gx) << 1) | ((v->y > gy) << 2) | ((-v->y > gy) << 3) |
           (v->w <= 0.0f ? OUTSIDE_BEHIND : 0);
}

static inline int outside_band(const GfxVertex *v) {
    return outside_band_of(v, sBandX, sBandY);
}

static float clip_distance(const GfxVertex *v, int plane) {
    switch (plane) {
        case 0: return sBandX * v->w - v->x;
        case 1: return sBandX * v->w + v->x;
        case 2: return sBandY * v->w - v->y;
        default: return sBandY * v->w + v->y;
    }
}

static void lerp_vertex(GfxVertex *out, const GfxVertex *a, const GfxVertex *b, float t) {
    out->x = a->x + (b->x - a->x) * t;
    out->y = a->y + (b->y - a->y) * t;
    out->z = a->z + (b->z - a->z) * t;
    out->w = a->w + (b->w - a->w) * t;
    out->u0 = a->u0 + (b->u0 - a->u0) * t;
    out->v0 = a->v0 + (b->v0 - a->v0) * t;
    out->u1 = a->u1 + (b->u1 - a->u1) * t;
    out->v1 = a->v1 + (b->v1 - a->v1) * t;
    out->r = (unsigned char) (a->r + (b->r - a->r) * t + 0.5f);
    out->g = (unsigned char) (a->g + (b->g - a->g) * t + 0.5f);
    out->b = (unsigned char) (a->b + (b->b - a->b) * t + 0.5f);
    out->a = (unsigned char) (a->a + (b->a - a->a) * t + 0.5f);
}

static int grow(void **buf, int *capacity, int needed, size_t size) {
    void *grown;
    int capacity2;
    if (needed <= *capacity) {
        return 1;
    }
    capacity2 = *capacity * 2 > needed ? *capacity * 2 : needed;
    grown = realloc(*buf, capacity2 * size);
    if (grown == NULL) {
        return 0;
    }
    *buf = grown;
    *capacity = capacity2;
    return 1;
}

/* The Screen page's Edge trimming row: 1 trims to the guard band, 0 sends triangles as the game made them. */
void plat_3ds_set_guard_band_setting(int on) {
    sGuardBand = on != 0;
    settings_set("guardband", sGuardBand);
}

static void __attribute__((noinline, cold)) read_guard_band_setting(void) {
    const char *env = plat_getenv("DKR_GUARD_BAND");
    sGuardBand = settings_get("guardband", env == NULL || atoi(env) != 0) != 0;
}

int plat_3ds_guard_band_setting(void) {
    if (__builtin_expect(sGuardBand < 0, 0)) {
        read_guard_band_setting();
    }
    return plat_switch("BENCH_GUARD_BAND", sGuardBand);
}

/*
 * Cuts one triangle (its corners a, b, c) to the guard band and writes the fan of the polygon that is left (at most 7
 * corners, so 5 triangles) to out, each triangle of the fan with its own three corners, as the triangle list before
 * indexing had. Returns how many corners were written (a multiple of 3, at most 15).
 */
static int __attribute__((noinline)) clip_triangle(const GfxVertex *a, const GfxVertex *b, const GfxVertex *c,
                                                   GfxVertex *out) {
    GfxVertex poly[2][16];
    int j, k, n, m = 0, plane;
    poly[0][0] = *a;
    poly[0][1] = *b;
    poly[0][2] = *c;
    n = 3;
    for (plane = 0; plane < 4 && n >= 3; plane++) {
        const GfxVertex *pin = poly[plane & 1];
        GfxVertex *pout = poly[(plane & 1) ^ 1];
        int cnt = 0;
        for (j = 0; j < n; j++) {
            const GfxVertex *p = &pin[j], *q = &pin[(j + 1) % n];
            float dp = clip_distance(p, plane), dq = clip_distance(q, plane);
            if (dp >= 0.0f) {
                pout[cnt++] = *p;
            }
            if ((dp >= 0.0f) != (dq >= 0.0f)) {
                lerp_vertex(&pout[cnt++], p, q, dp / (dp - dq));
            }
        }
        n = cnt;
    }
    // After four planes the polygon is back in poly[0]. Anything left with w <= 0 is behind the camera.
    for (k = 1; k + 1 < n; k++) {
        if (poly[0][0].w <= 0.0f || poly[0][k].w <= 0.0f || poly[0][k + 1].w <= 0.0f) {
            continue;
        }
        out[m] = poly[0][0];
        out[m + 1] = poly[0][k];
        out[m + 2] = poly[0][k + 1];
        m += 3;
    }
    return m;
}

/*
 * Clips the triangles that leave the guard band to it. Each vertex is tested once; when none is outside, the draw is
 * left as it is. Otherwise the vertices are copied, and the clipped triangles' new corners added after them, into
 * buffers of their own, which replace verts and indices: triangles within the band keep their indices, the others
 * are replaced by the fan of their clipped polygon (at most 7 corners, so 5 triangles).
 *
 * Roadmap P22: in a race about a third of the draws reach past the band and some 500 triangles a frame are outside it,
 * but more than 80% of those lie wholly beyond one of its sides (PC count), so clipping leaves nothing of them. With
 * DKR_FAST_CLIP (the default) they are dropped as soon as their corners' sides show it, and the vertices are only
 * copied once a triangle really has to be cut, which most draws never reach (their indices then point into the
 * draw's own vertices). What is drawn is the same.
 *
 * The cutting itself (from the first vertex found outside on) is out of line, so that only the test of the vertices
 * is in the per-draw path. With DKR_DRAW_LEAN that test is part of writing the vertices (gfxb_draw_indexed) and this
 * is only used for draws the lean path leaves to the full one.
 */
static void __attribute__((noinline)) clip_cut_triangles(const GfxVertex **verts, int *numVerts, const u16 **indices,
                                                         int *numTriangles) {
    const GfxVertex *in = *verts;
    const u16 *idx = *indices;
    int i, k, n, count = *numTriangles * 3, used, usedIdx, copied;

    // A triangle cut by all four planes has up to 7 corners: a fan of 5 triangles, each with its own 3 copies.
    if (!grow((void **) &sClipVerts, &sClipCapacity, *numVerts + *numTriangles * 15, sizeof(GfxVertex)) ||
        !grow((void **) &sClipIndices, &sClipIndexCapacity, *numTriangles * 15, sizeof(u16))) {
        return;
    }
    copied = !sFastClip;
    if (copied) {
        memcpy(sClipVerts, in, *numVerts * sizeof(GfxVertex));
    }
    used = *numVerts;
    usedIdx = 0;
    for (i = 0; i < count; i += 3) {
        int a = sClipOutside[idx[i]], b = sClipOutside[idx[i + 1]], c = sClipOutside[idx[i + 2]];
        if (!(a | b | c)) {
            sClipIndices[usedIdx++] = idx[i];
            sClipIndices[usedIdx++] = idx[i + 1];
            sClipIndices[usedIdx++] = idx[i + 2];
            continue;
        }
        if (sFastClip) {
            if (a & b & c & OUTSIDE_PLANES) {
                continue; // Wholly beyond one side: the clipper would leave nothing.
            }
            if (!copied) {
                memcpy(sClipVerts, in, *numVerts * sizeof(GfxVertex));
                copied = 1;
            }
        }
        n = clip_triangle(&in[idx[i]], &in[idx[i + 1]], &in[idx[i + 2]], &sClipVerts[used]);
        for (k = 0; k < n; k++) {
            sClipIndices[usedIdx++] = (u16) (used + k);
        }
        used += n;
    }
    if (copied) {
        *verts = sClipVerts;
        *numVerts = used;
    }
    *indices = sClipIndices;
    *numTriangles = usedIdx / 3;
}

static void clip_to_guard_band(const GfxVertex **verts, int *numVerts, const u16 **indices, int *numTriangles) {
    const GfxVertex *in = *verts;
    int i, any = 0;

    if (!sGuardBandFrame || !grow((void **) &sClipOutside, &sClipOutsideCapacity, *numVerts, 1)) {
        return;
    }
    // Clip x runs along the target's height (the screen's 400 or 800 pixels across), clip y along its width.
    sBandX = band_for(sTarget->frameBuf.height);
    sBandY = band_for(sTarget->frameBuf.width);
    for (i = 0; i < *numVerts; i++) {
        sClipOutside[i] = outside_band(&in[i]);
        any |= sClipOutside[i];
    }
    if (any) {
        clip_cut_triangles(verts, numVerts, indices, numTriangles);
    }
}

/*
 * The draws' parts (profile.h PROF_PART_BACKEND to _WRITE), timed while the profiler is on. They are kept in clock
 * ticks and handed over at the end of the frame: converting a reading to nanoseconds takes three 64-bit divisions,
 * which cost more than some of the parts (P23). As in the display list walk, each span between two readings holds
 * about one reading's cost, which is taken back out of its part and, through PROF_PART_WALK_TIMER, of the renderer's.
 * A reading is a system call (svcGetSystemTick), and a draw takes five to seven of them: on an Old 3DS, with its
 * 137 or so draws a frame, that is about half a millisecond as calibrated, and each one also evicts the draw's code
 * and data from the 16 KB caches. DKR_DRAW_PARTS=0 (the Old 3DS's default) takes no readings; the readings are out
 * of line and cold, so a draw without them carries one test per reading in its hot path.
 */
enum { DRAW_PART_BACKEND, DRAW_PART_CLIP, DRAW_PART_STATE, DRAW_PART_TWICE, DRAW_PART_WRITE, DRAW_PART_COUNT };
static const int sDrawPartProf[DRAW_PART_COUNT] = { PROF_PART_BACKEND, PROF_PART_CLIP, PROF_PART_STATE,
                                                     PROF_PART_STATE_TWICE, PROF_PART_WRITE };
static unsigned long long sDrawPartTicks[DRAW_PART_COUNT];
static unsigned int sDrawPartSpanReads[DRAW_PART_COUNT]; // Readings within each part's spans
static unsigned int sDrawPartReads;                      // All the readings taken

static inline void draw_part(int part, unsigned long long ticks, int reads) {
    sDrawPartTicks[part] += ticks;
    sDrawPartSpanReads[part] += reads;
}

/* A reading: the time since the reading before is charged to part (-1: none), with the readings within that span. */
static __attribute__((noinline, cold)) unsigned long long draw_reading(unsigned long long since, int part,
                                                                       int spanReads) {
    unsigned long long now = plat_ticks();
    if (part >= 0) {
        draw_part(part, now - since, spanReads);
    }
    return now;
}

/* The draw's last reading: the whole draw since its first one, and the readings the draw took in all. */
static __attribute__((noinline, cold)) void draw_readings_end(unsigned long long start, int spanReads, int reads) {
    draw_part(DRAW_PART_BACKEND, plat_ticks() - start, spanReads);
    sDrawPartReads += reads;
}

/* A draw that ends after its second reading with nothing left to draw: the whole of it was its first part (the
 * clipping, or the lean draw's vertex pass). */
static __attribute__((noinline, cold)) void draw_readings_empty(unsigned long long t0, unsigned long long t1) {
    draw_part(DRAW_PART_BACKEND, t1 - t0, 1);
    sDrawPartReads += 2;
}

static void hand_over_draw_parts(void) {
    unsigned long long cost, perSecond;
    int i;
    if (sDrawPartReads == 0) {
        return;
    }
    cost = prof_clock_read_cost();
    perSecond = plat_ticks_per_second();
    for (i = 0; i < DRAW_PART_COUNT; i++) {
        unsigned long long reads = sDrawPartSpanReads[i] * cost / 256;
        unsigned long long ticks = sDrawPartTicks[i] > reads ? sDrawPartTicks[i] - reads : 0;
        prof_part(sDrawPartProf[i], ticks * 1000000000ULL / perSecond);
        sDrawPartTicks[i] = 0;
        sDrawPartSpanReads[i] = 0;
    }
    prof_part(PROF_PART_WALK_TIMER, sDrawPartReads * cost / 256 * 1000000000ULL / perSecond);
    sDrawPartReads = 0;
}

/*
 * Indexed drawing (roadmap item P16): the frontend hands each vertex over once, however many triangles share it, so
 * it is converted and the GPU transforms it once. DKR_INDEXED=0 writes the triangles' corners one by one as before.
 */
static int sIndexed = -1;

static void __attribute__((noinline, cold)) read_indexed_setting(void) {
    const char *env = plat_getenv("DKR_INDEXED");
    sIndexed = env == NULL || atoi(env) != 0;
    plat_log("vertices: %s", sIndexed ? "shared, drawn by index" : "one for each triangle corner (DKR_INDEXED=0)");
}

static inline int indexed_enabled(void) {
    if (__builtin_expect(sIndexed < 0, 0)) {
        read_indexed_setting();
    }
    return sIndexed;
}

/* Converts one vertex to the GPU's short format at out (or the long one, with the second texture's coordinates
 * after it). Texture coordinates are scaled for padded textures, and t is flipped (the GPU's t = 0 is the last row).
 * scale3D is sScale3D, passed in so that a loop can keep it in a register (see test_and_write_vertices). */
static inline void convert_vertex(const GfxVertex *v, u8 *out, int fmt, int flat, float scale3D, float su0,
                                  float sv0, float su1, float sv1) {
    C3DVertex *o = (C3DVertex *) out;
    if (flat) {
        to_screen_flat(v, o->pos, flat);
    } else {
        to_screen_scaled(v, o->pos, scale3D);
    }
    o->tc0[0] = v->u0 * su0;
    o->tc0[1] = 1.0f - v->v0 * sv0;
    memcpy(o->color, &v->r, 4);
    if (fmt == VFMT_LONG) {
        C3DVertex2 *o2 = (C3DVertex2 *) out;
        o2->tc1[0] = v->u1 * su1;
        o2->tc1[1] = 1.0f - v->v1 * sv1;
    }
}

/* The 48-byte vertices of DKR_VERTEX_FORMAT=48, kept out of the usual path. */
static void __attribute__((noinline, cold)) write_vertices_old(u8 *out, const GfxVertex *verts, int count,
                                                               const u16 *indices, int flat, float su0, float sv0,
                                                               float su1, float sv1) {
    C3DVertexOld *o = (C3DVertexOld *) out;
    int i;
    for (i = 0; i < count; i++) {
        const GfxVertex *v = &verts[indices != NULL ? indices[i] : i];
        place(v, o[i].pos, flat);
        o[i].tc0[0] = v->u0 * su0;
        o[i].tc0[1] = 1.0f - v->v0 * sv0;
        o[i].tc1[0] = v->u1 * su1;
        o[i].tc1[1] = 1.0f - v->v1 * sv1;
        o[i].color[0] = v->r;
        o[i].color[1] = v->g;
        o[i].color[2] = v->b;
        o[i].color[3] = v->a;
    }
}

/* Writes the vertices in format fmt at out, stride apart, or if indices is given the vertices they name in order. */
static void write_vertices(int fmt, u8 *out, const GfxVertex *verts, int count, const u16 *indices, int flat, float su0,
                           float sv0, float su1, float sv1) {
    int stride = sVfmtStride[fmt], i;
    if (fmt == VFMT_OLD) {
        write_vertices_old(out, verts, count, indices, flat, su0, sv0, su1, sv1);
        return;
    }
    for (i = 0; i < count; i++) {
        const GfxVertex *v = &verts[indices != NULL ? indices[i] : i];
        convert_vertex(v, out + i * stride, fmt, flat, sScale3D, su0, sv0, su1, sv1);
    }
}

/* The frame's buffer (vertices, indices or commands) is full: logs it, for the first few draws left out. */
static void __attribute__((noinline, cold)) log_draw_left_out(int vertBytes, int count, int indexed) {
    if (sDrawsLeftOut++ < 5) {
        plat_log("frame %d: %s full (%d bytes of vertices, %d indices, %lu command words), triangles left out",
                 sFrameCount,
                 sVboUsed + 15 + vertBytes > VBO_BYTES ? "vertex buffer"
                 : indexed && sIboUsed + count > IBO_BYTES / (int) sizeof(u16) ? "index buffer"
                                                                                   : "command list",
                 sVboUsed, sIboUsed, (unsigned long) cmd_position());
    }
}

/* DKR_LOG_DRAWS (or a snapshot): logs a draw, and checks its texture against what was uploaded. verts holds numVerts
 * vertices; the lean draw's cut corners are only in the vertex buffer, so its first index may be past them. */
static void __attribute__((noinline, cold)) log_draw(const GfxDrawState *state, const GfxVertex *verts, int numVerts,
                                                     int written, const u16 *indices, int numTriangles) {
    const GfxCombiner *c = &state->combiner;
    const GfxTexture *tex = state->textures[0];
    const C3DTexture *t = (tex != NULL) ? (const C3DTexture *) tex->backendData : NULL;
    const GfxVertex *v0 = &verts[indices[0] < numVerts ? indices[0] : 0];
    if (tex != NULL) {
        verify_texture(tex);
        if (t != NULL) {
            remember_texture(t);
        }
    }
    plat_log("draw %d tris %d verts blend %d at %d/%d z %d/%d dec %d fog %d cull %d cc %d,%d,%d,%d/%d,%d,%d,%d "
             "cyc %d tex %p (%dx%d gpu %dx%d at %p param %08lX) v0 %.3f %.3f %.3f %.3f uv %.3f %.3f rgba %d %d %d "
             "%d cmd %lu",
             numTriangles, written, state->blend, state->alphaTest, state->alphaRef, state->depthTest,
             state->depthWrite, state->decal, state->fogEnabled, state->cullMode, c->rgb[0].a, c->rgb[0].b,
             c->rgb[0].c, c->rgb[0].d, c->alpha[0].a, c->alpha[0].b, c->alpha[0].c, c->alpha[0].d, c->numCycles,
             (void *) tex, tex != NULL ? tex->width : 0, tex != NULL ? tex->height : 0,
             t != NULL ? t->tex.width : 0, t != NULL ? t->tex.height : 0, t != NULL ? t->tex.data : NULL,
             (unsigned long) (t != NULL ? t->tex.param : 0), v0->x, v0->y, v0->z, v0->w, v0->u0, v0->v0, v0->r,
             v0->g, v0->b, v0->a, (unsigned long) gpuCmdBufOffset);
}

static void __attribute__((noinline, cold)) read_log_draws_setting(void) {
    const char *env = plat_getenv("DKR_LOG_DRAWS");
    sLogFrame = env != NULL ? atoi(env) : -1;
}

/* A draw's texture coordinate scales (bind_texture's), without binding anything. */
static inline void texture_scales(const GfxTexture *tex, float *scaleU, float *scaleV) {
    const C3DTexture *t = (tex != NULL) ? (const C3DTexture *) tex->backendData : NULL;
    *scaleU = (t != NULL) ? t->scaleU : 1.0f;
    *scaleV = (t != NULL) ? t->scaleV : 1.0f;
}

/* The draw as it was before DKR_DRAW_LEAN (below): the guard band test, then the settings, then the vertices. */
static void __attribute__((noinline)) draw_indexed_full(const GfxDrawState *state, const GfxVertex *verts,
                                                        int numVerts, const u16 *indices, int numTriangles) {
    HangDraw *hangDraw;
    float su0, sv0, su1, sv1;
    int first, vertex, count, fmt, flat = gfx_flat_mode(state), indexed = indexed_enabled();
    int vertBytes, written;
    u16 *ibo = NULL;
    // Timed in parts while the profiler is on and DKR_DRAW_PARTS says so (draw_part).
    int timed = sDrawParts && prof_enabled(), twiceReads = 0;
    unsigned long long t0 = 0, t1 = 0, t2 = 0;

    if (timed) {
        t0 = draw_reading(0, -1, 0);
    }
    if (sStereo && !flat && sTuning[STEREO_AUTO]) {
        // The draw's own vertices: the clip below adds corners on the near plane, nearer than anything really is.
        near_depth_note(verts, numVerts);
    }
    clip_to_guard_band(&verts, &numVerts, &indices, &numTriangles);
    if (timed) {
        t1 = draw_reading(t0, DRAW_PART_CLIP, 1);
    }
    if (numTriangles == 0) {
        if (timed) {
            draw_readings_empty(t0, t1);
        }
        return;
    }
    count = numTriangles * 3;
    // Coordinates for a second texture only when the draw binds one (else it is the white texture).
    fmt = sVfmtOld ? VFMT_OLD : state->textures[1] != NULL ? VFMT_LONG : VFMT_SHORT;
    written = indexed ? numVerts : count; // Vertices written to the buffer
    vertBytes = written * sVfmtStride[fmt];

    if (sVboUsed + 15 + vertBytes > VBO_BYTES || (indexed && sIboUsed + count > IBO_BYTES / (int) sizeof(u16)) ||
        gpuCmdBufSize - gpuCmdBufOffset < sCmdWords / 16) {
        // Out of room in the frame's vertex buffer or command list (a full command list stops the program, and
        // nothing of the frame reaches the GPU before it ends, so splitting it would not make room): leave the rest
        // of the frame's triangles out. Races use under a fifth of either.
        log_draw_left_out(vertBytes, count, indexed);
        if (timed) {
            sDrawPartReads += 2;
        }
        return;
    }
    if (__builtin_expect(sLogFrame == -2, 0)) {
        read_log_draws_setting();
    }
    if (__builtin_expect(sFrameCount == sLogFrame, 0)) {
        log_draw(state, verts, numVerts, numVerts, indices, numTriangles);
    }
    hangDraw = hang_draw_start(state, numTriangles);
    first = drain_before_draw(state, numTriangles);
    if (first) {
        forget_gpu_state();
    }
    set_all_state(state, &su0, &sv0, &su1, &sv1);
    if (first) {
        unsigned long long tw = timed ? draw_reading(0, -1, 0) : 0;
        write_settings_twice(state, &su0, &sv0, &su1, &sv1);
        if (timed) {
            draw_reading(tw, DRAW_PART_TWICE, 1);
            twiceReads = 2;
        }
    }

    set_cpu_uniforms();
    unif_write_twice();
    if (timed) {
        t2 = draw_reading(t1, DRAW_PART_STATE, 1 + twiceReads);
    }
    vertex = use_vertex_format(fmt);
    if (indexed && vertex + numVerts > 0xFFFF) {
        // The indices are 16 bits from the start of the buffer setting: start a new one here.
        sVfmtBound = -1;
        vertex = use_vertex_format(fmt);
    }
    write_vertices(fmt, sVbo + sVboUsed, verts, written, indexed ? NULL : indices, flat, su0, sv0, su1, sv1);
    if (indexed) {
        int i;
        ibo = sIbo + sIboUsed;
        for (i = 0; i < count; i++) {
            ibo[i] = (u16) (vertex + indices[i]);
        }
        sIboUsed += count;
    }
    if (timed) {
        draw_reading(t2, DRAW_PART_WRITE, 1);
    }
    draw_prims(vertex, count, ibo);
    if (hangDraw != NULL) {
        hangDraw->cmdEnd = cmd_position();
    }
    sVboUsed += vertBytes;
    sFrameTriangles += numTriangles;
    if (timed) {
        draw_readings_end(t0, 4 + twiceReads, 5 + twiceReads);
    }
}

/*
 * The lean draw (DKR_DRAW_LEAN, on by default on an Old 3DS). The draw above went over a draw's vertices up to three
 * times: the guard band test read each one, a draw with a cut triangle copied them all, and writing them to the
 * vertex buffer read them again; and the code for all of that, with its cold branches, was far more than the Old
 * 3DS's 16 KB instruction cache holds along with the display list walk, so each draw fetched much of it from memory
 * again. Here each vertex is read once: it is tested against the band and written to the vertex buffer in the same
 * pass, where the draw's vertices would go. Only when a vertex is outside the band are the triangles gone over
 * (clip_cut_lean, out of line): the corners of those that are cut are written after the draw's own vertices, exactly
 * as the full draw appended them to its copy, so the vertex and index buffers come out the same bytes, and the
 * settings are written in the same order. What the full draw does that this one doesn't (the 48-byte vertex format,
 * DKR_INDEXED=0, edge trimming off, or a frame's buffer nearly full) is left to it.
 */
static u8 sLeanOutside[GFX_MAX_BATCH_VERTICES];           // Each vertex's sides of the band (outside_band)
static u16 sLeanIndices[GFX_MAX_BATCH_TRIANGLES * 15];   // The indices of a draw with a vertex outside the band

/*
 * The triangles of a draw with a vertex outside the band: those within it keep their indices, those wholly beyond
 * one side are dropped (DKR_FAST_CLIP) and the rest are cut, their fans' corners converted after the draw's own
 * vertices (used of them so far at out). Returns the vertex count; *count gets the index count.
 */
static int __attribute__((noinline)) clip_cut_lean(const GfxVertex *verts, const u16 *idx, int numTriangles, u8 *out,
                                                   int stride, int fmt, int flat, float su0, float sv0, float su1,
                                                   float sv1, int used, int *count) {
    int i, k, n, usedIdx = 0, indexCount = numTriangles * 3;
    for (i = 0; i < indexCount; i += 3) {
        int a = sLeanOutside[idx[i]], b = sLeanOutside[idx[i + 1]], c = sLeanOutside[idx[i + 2]];
        if (!(a | b | c)) {
            sLeanIndices[usedIdx++] = idx[i];
            sLeanIndices[usedIdx++] = idx[i + 1];
            sLeanIndices[usedIdx++] = idx[i + 2];
            continue;
        }
        if (sFastClip && (a & b & c & OUTSIDE_PLANES)) {
            continue; // Wholly beyond one side: the clipper would leave nothing.
        }
        {
            GfxVertex fan[15];
            n = clip_triangle(&verts[idx[i]], &verts[idx[i + 1]], &verts[idx[i + 2]], fan);
            for (k = 0; k < n; k++) {
                convert_vertex(&fan[k], out + (used + k) * stride, fmt, flat, sScale3D, su0, sv0, su1, sv1);
                sLeanIndices[usedIdx++] = (u16) (used + k);
            }
            used += n;
        }
    }
    *count = usedIdx;
    return used;
}

/*
 * The lean draw's one pass over the vertices: each is tested against the band (its sides noted in outside, and all
 * of them or'ed into the result) and written to the vertex buffer. The band and the 3D scale are copied out of their
 * variables first, as the compiler reloads those after every vertex written otherwise (`out` could point at them).
 * The empty asm keeps the compiler from taking the flat and format branches out of the loop into a copy of it for
 * each case (three copies of this loop, of which a frame's draws use all three): one copy with the branches in it
 * costs each vertex two predictable branches and keeps the per-draw code a few cache lines shorter. The same for the
 * stereo and wide screen settings the flat path reads, which the loop isn't unswitched on either (the attribute: an
 * asm on them too would cost the loop register moves). Out of line too: inlined, the loop came out twice in
 * gfxb_draw_indexed.
 */
PLAT_HOT static int __attribute__((noinline, optimize("no-unswitch-loops")))
test_and_write_vertices(const GfxVertex *verts, u8 *out, u8 *outside, int numVerts, int stride, int fmt, int flat,
                        float su0, float sv0, float su1, float sv1) {
    const float bandX = sBandX, bandY = sBandY, scale3D = sScale3D;
    int i, any = 0;
    for (i = 0; i < numVerts; i++) {
        const GfxVertex *v = &verts[i];
        int b = outside_band_of(v, bandX, bandY);
        __asm__("" : "+r"(flat), "+r"(fmt));
        outside[i] = (u8) b;
        any |= b;
        convert_vertex(v, out + i * stride, fmt, flat, scale3D, su0, sv0, su1, sv1);
    }
    return any;
}

/* The buffer setting has 16-bit indices from its start, and the lean draw's vertices (already written at out) would
 * reach past them: starts a new setting here, and moves them where it starts. Rare (races use under a fifth of the
 * buffer), so out of the per-draw code. */
static int __attribute__((noinline, cold)) restart_vertex_buffer(int fmt, const u8 *out, int bytes) {
    int vertex;
    sVfmtBound = -1;
    vertex = use_vertex_format(fmt);
    if (sVbo + sVboUsed != out) {
        memmove(sVbo + sVboUsed, out, bytes); // Only after the new buffer setting moved the start.
    }
    return vertex;
}

PLAT_HOT void gfxb_draw_indexed(const GfxDrawState *state, const GfxVertex *verts, int numVerts, const u16 *indices,
                       int numTriangles) {
    HangDraw *hangDraw;
    float su0, sv0, su1, sv1;
    int first, vertex, count, fmt, stride, flat, start, written, any, i;
    u8 *out;
    u16 *ibo;
    // Timed in parts while the profiler is on and DKR_DRAW_PARTS says so (draw_part). Here the vertex pass, band test
    // and cutting included, is counted as writing the vertices (PROF_PART_WRITE), and PROF_PART_CLIP stays empty.
    int timed, twiceReads = 0;
    unsigned long long t0 = 0, t1 = 0, t2 = 0;

    // Coordinates for a second texture only when the draw binds one (else it is the white texture).
    fmt = state->textures[1] != NULL ? VFMT_LONG : VFMT_SHORT;
    stride = sVfmtStride[fmt];
    // In 3D with auto convergence the full draw notes each draw's nearest vertices first (near_depth_note).
    if (!sDrawLean || sVfmtOld || !indexed_enabled() || !sGuardBandFrame ||
        (sStereo && sTuning[STEREO_AUTO]) || numVerts > GFX_MAX_BATCH_VERTICES || numTriangles > GFX_MAX_BATCH_TRIANGLES ||
        // Room for the most that cutting can add (the full draw checks what it really needs, and may still draw).
        sVboUsed + 15 + (numVerts + numTriangles * 15) * stride > VBO_BYTES ||
        sIboUsed + numTriangles * 15 > IBO_BYTES / (int) sizeof(u16) ||
        gpuCmdBufSize - gpuCmdBufOffset < sCmdWords / 16) {
        draw_indexed_full(state, verts, numVerts, indices, numTriangles);
        return;
    }
    timed = sDrawParts && prof_enabled();
    if (timed) {
        t0 = draw_reading(0, -1, 0);
    }
    flat = gfx_flat_mode(state);
    texture_scales(state->textures[0], &su0, &sv0);
    texture_scales(state->textures[1], &su1, &sv1);
    // Where use_vertex_format will start the vertices: the same format carries on, a new one starts 16-byte aligned.
    start = fmt != sVfmtBound ? (sVboUsed + 15) & ~15 : sVboUsed;
    out = sVbo + start;
    any = test_and_write_vertices(verts, out, sLeanOutside, numVerts, stride, fmt, flat, su0, sv0, su1, sv1);
    written = numVerts;
    count = numTriangles * 3;
    if (count == 0) { // Nothing to draw (no caller passes 0 today): touch no GPU state, as the full path does not
        if (timed) {
            t1 = draw_reading(t0, DRAW_PART_WRITE, 1);
            draw_readings_empty(t0, t1);
        }
        return;
    }
    if (__builtin_expect(any, 0)) {
        written = clip_cut_lean(verts, indices, numTriangles, out, stride, fmt, flat, su0, sv0, su1, sv1, numVerts,
                                &count);
        indices = sLeanIndices;
        numTriangles = count / 3;
        if (count == 0) {
            if (timed) {
                t1 = draw_reading(t0, DRAW_PART_WRITE, 1);
                draw_readings_empty(t0, t1);
            }
            return;
        }
    }
    if (timed) {
        t1 = draw_reading(t0, DRAW_PART_WRITE, 1);
    }
    if (__builtin_expect(sLogFrame == -2, 0)) {
        read_log_draws_setting();
    }
    if (__builtin_expect(sFrameCount == sLogFrame, 0)) {
        log_draw(state, verts, numVerts, written, indices, numTriangles);
    }
    hangDraw = hang_draw_start(state, numTriangles);
    first = drain_before_draw(state, numTriangles);
    if (first) {
        forget_gpu_state();
    }
    set_all_state(state, &su0, &sv0, &su1, &sv1);
    if (first) {
        unsigned long long tw = timed ? draw_reading(0, -1, 0) : 0;
        write_settings_twice(state, &su0, &sv0, &su1, &sv1);
        if (timed) {
            draw_reading(tw, DRAW_PART_TWICE, 1);
            twiceReads = 2;
        }
    }
    // With GPU vertex maths on, the one shader also draws the CPU path: after a model draw its
    // uniforms must be set back to the identity, exactly as the full draw above does.
    set_cpu_uniforms();
    unif_write_twice();
    if (timed) {
        t2 = draw_reading(t1, DRAW_PART_STATE, 1 + twiceReads);
    }
    vertex = use_vertex_format(fmt);
    if (__builtin_expect(vertex + written > 0xFFFF, 0)) {
        vertex = restart_vertex_buffer(fmt, out, written * stride);
    }
    ibo = sIbo + sIboUsed;
    for (i = 0; i < count; i++) {
        ibo[i] = (u16) (vertex + indices[i]);
    }
    sIboUsed += count;
    if (timed) {
        draw_reading(t2, DRAW_PART_WRITE, 1);
    }
    if (sStereo) {
        draw_prims_stereo(vertex, count, ibo);
    } else {
        C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, ibo);
    }
    if (hangDraw != NULL) {
        hangDraw->cmdEnd = cmd_position();
    }
    sVboUsed += written * stride;
    sFrameTriangles += numTriangles;
    if (timed) {
        draw_readings_end(t0, 4 + twiceReads, 5 + twiceReads);
    }
}

/* A triangle list: each corner its own vertex. */
void gfxb_draw_triangles(const GfxDrawState *state, const GfxVertex *verts, int numTriangles) {
    static u16 sIdentity[GFX_MAX_BATCH_VERTICES];
    int i;
    if (numTriangles * 3 > GFX_MAX_BATCH_VERTICES) {
        numTriangles = GFX_MAX_BATCH_VERTICES / 3;
    }
    if (sIdentity[1] == 0) {
        for (i = 0; i < GFX_MAX_BATCH_VERTICES; i++) {
            sIdentity[i] = (u16) i;
        }
    }
    gfxb_draw_indexed(state, verts, numTriangles * 3, sIdentity, numTriangles);
}

/*
 * GPU vertex maths (P30): whether a model draw can go to the GPU. The CPU path trims triangles to the guard band
 * (clip_to_guard_band), which needs their positions; here only the corners of the vertices' box are transformed. A box
 * wholly beyond one side of the band is drawn by neither path; one reaching past the band, or behind the camera, is
 * left to the CPU path.
 */
int gfxb_model_check(const float mtx[4][4], const short boxMin[3], const short boxMax[3]) {
    int all = ~0, any = 0, i;
    if (!sTransformOn) {
        return GFX_MODEL_CPU;
    }
    if (!sGuardBandFrame) {
        return GFX_MODEL_GPU;
    }
    sBandX = band_for(sTarget->frameBuf.height);
    sBandY = band_for(sTarget->frameBuf.width);
    for (i = 0; i < 8; i++) {
        float x = (i & 1) ? boxMax[0] : boxMin[0], y = (i & 2) ? boxMax[1] : boxMin[1], z = (i & 4) ? boxMax[2] : boxMin[2];
        GfxVertex c;
        int out;
        c.x = x * mtx[0][0] + y * mtx[1][0] + z * mtx[2][0] + mtx[3][0];
        c.y = x * mtx[0][1] + y * mtx[1][1] + z * mtx[2][1] + mtx[3][1];
        c.w = x * mtx[0][3] + y * mtx[1][3] + z * mtx[2][3] + mtx[3][3];
        out = outside_band(&c);
        all &= out;
        any |= out;
    }
    return (all & OUTSIDE_PLANES) ? GFX_MODEL_HIDDEN : any ? GFX_MODEL_CPU : GFX_MODEL_GPU;
}

/* What a model or mesh draw does before its vertices: room, hang report, settings, uniforms. Returns FALSE if the draw
 * is to be left out (no room left in the frame). */
static int model_draw_begin(const GfxDrawState *state, const GfxModelParams *params, int vertBytes, int numIndices,
                            int numTriangles, HangDraw **hangDraw) {
    float su0, sv0, su1, sv1;
    int first;
    if (sVboUsed + 15 + vertBytes > VBO_BYTES || sIboUsed + numIndices > IBO_BYTES / (int) sizeof(u16) ||
        gpuCmdBufSize - gpuCmdBufOffset < sCmdWords / 16) {
        if (sDrawsLeftOut++ < 5) {
            plat_log("frame %d: buffers full (%d bytes of vertices, %d indices, %lu command words), model triangles "
                     "left out", sFrameCount, sVboUsed, sIboUsed, (unsigned long) cmd_position());
        }
        return 0;
    }
    *hangDraw = hang_draw_start(state, numTriangles);
    first = drain_before_draw(state, numTriangles);
    if (first) {
        forget_gpu_state();
    }
    set_all_state(state, &su0, &sv0, &su1, &sv1);
    if (first) {
        write_settings_twice(state, &su0, &sv0, &su1, &sv1);
    }
    set_model_uniforms(params, su0, sv0, su1, sv1);
    unif_write_twice();
    return 1;
}

void gfxb_draw_model(const GfxDrawState *state, const GfxModelParams *params, const GfxModelVertex *verts, int numVerts,
                     const unsigned short *indices, int numTriangles) {
    HangDraw *hangDraw;
    int vertex, i, count = numTriangles * 3, vertBytes = numVerts * (int) sizeof(GfxModelVertex);
    unsigned long long t0 = prof_enabled() ? plat_ticks() : 0;
    u16 *ibo;

    if (!model_draw_begin(state, params, vertBytes, count, numTriangles, &hangDraw)) {
        return;
    }
    vertex = use_vertex_format(VFMT_MODEL);
    if (vertex + numVerts > 0xFFFF) {
        sVfmtBound = -1;
        vertex = use_vertex_format(VFMT_MODEL);
    }
    memcpy(sVbo + sVboUsed, verts, vertBytes);
    ibo = sIbo + sIboUsed;
    for (i = 0; i < count; i++) {
        ibo[i] = (u16) (vertex + indices[i]);
    }
    sIboUsed += count;
    draw_prims(vertex, count, ibo);
    if (hangDraw != NULL) {
        hangDraw->cmdEnd = cmd_position();
    }
    sVboUsed += vertBytes;
    sFrameTriangles += numTriangles;
    if (t0 != 0) {
        draw_part(DRAW_PART_BACKEND, plat_ticks() - t0, 2);
        sDrawPartReads += 2;
    }
}

/*
 * Meshes (P31, P34). Their vertices live in one arena of linear memory, MESH_ARENA_VERTS model vertices, so that the
 * GPU reads any mesh from the same vertex buffer setting with 16-bit indices: gfxb_draw_meshes then draws parts of
 * many meshes as one draw, copying their indices (made relative to the arena when the mesh is created) into the
 * frame's index buffer. The arena is handed out in runs of vertices, first fit, from a list of free runs.
 */
#define MESH_ARENA_VERTS 65536
#define MESH_FREE_RUNS 2048

typedef struct C3DMesh {
    u32 base, numVerts;
    u16 *indices; // Arena indices
} C3DMesh;

static GfxModelVertex *sMeshArena;
static struct {
    u32 start, count;
} sArenaFree[MESH_FREE_RUNS];
static int sArenaFreeRuns;

static int arena_alloc(u32 count, u32 *start) {
    int i;
    if (sMeshArena == NULL) {
        sMeshArena = linearAlloc(MESH_ARENA_VERTS * sizeof(GfxModelVertex));
        if (sMeshArena == NULL) {
            return 0;
        }
        sArenaFree[0].start = 0;
        sArenaFree[0].count = MESH_ARENA_VERTS;
        sArenaFreeRuns = 1;
        plat_log("meshes: %d KB arena", (int) (MESH_ARENA_VERTS * sizeof(GfxModelVertex) / 1024));
    }
    for (i = 0; i < sArenaFreeRuns; i++) {
        if (sArenaFree[i].count >= count) {
            *start = sArenaFree[i].start;
            sArenaFree[i].start += count;
            sArenaFree[i].count -= count;
            if (sArenaFree[i].count == 0) {
                memmove(&sArenaFree[i], &sArenaFree[i + 1], (sArenaFreeRuns - i - 1) * sizeof(sArenaFree[0]));
                sArenaFreeRuns--;
            }
            return 1;
        }
    }
    return 0;
}

/* Gives a run back, joined to its free neighbours. With no room left in the list it is lost (logged once). */
static void arena_free(u32 start, u32 count) {
    int i = 0;
    while (i < sArenaFreeRuns && sArenaFree[i].start < start) {
        i++;
    }
    if (i > 0 && sArenaFree[i - 1].start + sArenaFree[i - 1].count == start) {
        sArenaFree[i - 1].count += count;
        if (i < sArenaFreeRuns && start + count == sArenaFree[i].start) {
            sArenaFree[i - 1].count += sArenaFree[i].count;
            memmove(&sArenaFree[i], &sArenaFree[i + 1], (sArenaFreeRuns - i - 1) * sizeof(sArenaFree[0]));
            sArenaFreeRuns--;
        }
        return;
    }
    if (i < sArenaFreeRuns && start + count == sArenaFree[i].start) {
        sArenaFree[i].start = start;
        sArenaFree[i].count += count;
        return;
    }
    if (sArenaFreeRuns == MESH_FREE_RUNS) {
        static int sWarned;
        if (!sWarned) {
            sWarned = 1;
            plat_log("meshes: arena free list full, a run is lost");
        }
        return;
    }
    memmove(&sArenaFree[i + 1], &sArenaFree[i], (sArenaFreeRuns - i) * sizeof(sArenaFree[0]));
    sArenaFree[i].start = start;
    sArenaFree[i].count = count;
    sArenaFreeRuns++;
}

void *gfxb_mesh_create(const GfxModelVertex *verts, int numVerts, const unsigned short *indices, int numIndices) {
    C3DMesh *m = malloc(sizeof(C3DMesh));
    int i;
    if (m == NULL || (m->indices = malloc(numIndices * sizeof(u16))) == NULL) {
        free(m);
        return NULL;
    }
    if (!arena_alloc(numVerts, &m->base)) {
        free(m->indices);
        free(m);
        return NULL;
    }
    m->numVerts = numVerts;
    memcpy(&sMeshArena[m->base], verts, numVerts * sizeof(GfxModelVertex));
    for (i = 0; i < numIndices; i++) {
        m->indices[i] = (u16) (m->base + indices[i]);
    }
    gpu_flush(&sMeshArena[m->base], numVerts * sizeof(GfxModelVertex));
    return m;
}

/* The GPU may still draw the mesh in the frames it was handed: deleted once it has (process_pending_frees). */
void gfxb_mesh_free(void *mesh) {
    FrameSlot *slot = &sSlots[sRec];
    if (mesh == NULL) {
        return;
    }
    if (slot->numMeshFrees == slot->meshFreesCapacity) {
        int capacity = slot->meshFreesCapacity ? slot->meshFreesCapacity * 2 : 64;
        void **list = realloc(slot->meshFrees, capacity * sizeof(void *));
        if (list == NULL) {
            return; // Out of memory: the mesh is kept (a leak, but nothing the GPU reads is freed).
        }
        slot->meshFrees = list;
        slot->meshFreesCapacity = capacity;
    }
    slot->meshFrees[slot->numMeshFrees++] = mesh;
}

static void delete_mesh(void *mesh) {
    C3DMesh *m = mesh;
    arena_free(m->base, m->numVerts);
    if (sArenaFreeRuns == 1 && sArenaFree[0].count == MESH_ARENA_VERTS) {
        // The last mesh is gone (meshes turned off, or a level left): the arena's megabyte goes back to the linear
        // heap, which an Old 3DS needs for textures. Every frame that drew from it is done, as frees wait for that.
        linearFree(sMeshArena);
        sMeshArena = NULL;
        sArenaFreeRuns = 0;
        if (sBufMesh != NULL) {
            sBufMesh = NULL; // The vertex buffer setting pointed into it: the next mesh draw points it again
        }
        plat_log("meshes: arena freed");
    }
    free(m->indices);
    free(m);
}

void gfxb_draw_meshes(const GfxDrawState *state, const GfxModelParams *params, const GfxMeshPart *parts,
                      int numParts) {
    HangDraw *hangDraw;
    unsigned long long t0 = prof_enabled() ? plat_ticks() : 0;
    int i, count = 0;
    u16 *ibo;

    for (i = 0; i < numParts; i++) {
        count += parts[i].numTriangles * 3;
    }
    if (count == 0 || !model_draw_begin(state, params, 0, count, count / 3, &hangDraw)) {
        return;
    }
    if (sBufMesh != sMeshArena) {
        // The vertex buffer setting points at the arena until a draw from the frame's buffer points it back.
        C3D_BufInfo *buf = C3D_GetBufInfo();
        if (sVfmtAttrSet != VFMT_MODEL) {
            C3D_SetAttrInfo(&sVfmtAttr[VFMT_MODEL]);
            sVfmtAttrSet = VFMT_MODEL;
        }
        BufInfo_Init(buf);
        BufInfo_Add(buf, sMeshArena, sizeof(GfxModelVertex), 3, 0x210);
        sBufMesh = sMeshArena;
        sVfmtBound = -1;
    }
    ibo = sIbo + sIboUsed;
    for (i = 0, count = 0; i < numParts; i++) {
        const C3DMesh *m = parts[i].mesh;
        memcpy(ibo + count, m->indices + parts[i].firstIndex, parts[i].numTriangles * 3 * sizeof(u16));
        count += parts[i].numTriangles * 3;
    }
    sIboUsed += count;
    draw_prims(0, count, ibo);
    if (hangDraw != NULL) {
        hangDraw->cmdEnd = cmd_position();
    }
    sFrameTriangles += count / 3;
    if (t0 != 0) {
        draw_part(DRAW_PART_BACKEND, plat_ticks() - t0, 2);
        sDrawPartReads += 2;
    }
}

/* Draws a screen rectangle with a flat colour, optionally only writing depth. */
static void draw_clear_rect(const short rect[4], u32 rgba, int depthOnly) {
    GfxDrawState state;
    GfxVertex v[6];
    float x0 = rect[0] / (GFX_SCREEN_WIDTH / 2.0f) - 1.0f, x1 = rect[2] / (GFX_SCREEN_WIDTH / 2.0f) - 1.0f;
    float y0 = 1.0f - rect[1] / (GFX_SCREEN_HEIGHT / 2.0f), y1 = 1.0f - rect[3] / (GFX_SCREEN_HEIGHT / 2.0f);
    const float xs[6] = { x0, x0, x1, x1, x0, x1 };
    const float ys[6] = { y0, y1, y0, y0, y1, y1 };
    int i;

    memset(&state, 0, sizeof(state));
    state.combiner.numCycles = 1;
    state.combiner.rgb[0].a = state.combiner.rgb[0].b = state.combiner.rgb[0].c = GFX_CC_ZERO;
    state.combiner.rgb[0].d = GFX_CC_SHADE;
    state.combiner.alpha[0] = state.combiner.rgb[0];
    state.scissor[0] = rect[0];
    state.scissor[1] = rect[1];
    state.scissor[2] = rect[2];
    state.scissor[3] = rect[3];
    state.flat = 1;
    for (i = 0; i < 6; i++) {
        memset(&v[i], 0, sizeof(v[i]));
        v[i].x = xs[i];
        v[i].y = ys[i];
        v[i].z = 1.0f; // The far plane, so a depth clear writes the farthest depth.
        v[i].w = 1.0f;
        v[i].r = rgba >> 24;
        v[i].g = rgba >> 16;
        v[i].b = rgba >> 8;
        v[i].a = rgba;
    }
    sDepthOnly = depthOnly;
    sClearing = 1;
    gfxb_draw_triangles(&state, v, 2);
    if (depthOnly && sEarlyDepth && !sStereo) {
        // The early depth buffer is cleared with the depth buffer, to the farthest depth (0, as in the depth buffer).
        GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR, 1);
    }
    sDepthOnly = 0;
    sClearing = 0;
}

void gfxb_clear_color(const short rect[4], unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    // Colour fills are drawn so that they cover only the game's part of the screen, leaving black bars at the sides.
    draw_clear_rect(rect, (r << 24) | (g << 16) | (b << 8) | a, 0);
}

/*
 * Each frame starts with a depth clear drawn over the whole screen, and the game draws its own right after (roadmap
 * item P18): two passes over every pixel. Instead, a whole-screen depth clear before anything else is drawn becomes
 * one memory fill of the depth buffer, queued ahead of the frame's command list in gfxb_end_frame. A depth-only fill
 * was blamed for hanging a real New 3DS after the first frame in October, but that hang turned out to be the vertex
 * shader, which hung the GPU with plain drawn clears too; switch bench #176 ran it on a New 3DS without trouble and
 * saved 0.4-1.6 ms of GPU time a frame. Other depth clears (part of the screen, or after a draw) are drawn.
 * DKR_DEPTH_FILL=0 draws them all, as before.
 */
void gfxb_clear_depth(const short rect[4]) {
    // The game's own clear leaves out the bottom row (its scissor ends at 239), which the frame's first clear has
    // just cleared anyway.
    if (sDepthFill && sFrameFirstDraw && rect[0] <= 0 && rect[1] <= 0 && rect[2] >= GFX_SCREEN_WIDTH - 1 &&
        rect[3] >= GFX_SCREEN_HEIGHT - 1) {
        sDepthFillPending = 1;
        if (sEarlyDepth && !sStereo) {
            GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR, 1);
        }
        return;
    }
    draw_clear_rect(rect, 0, 1);
}
