/*
 * citro3d (PICA200) rendering backend for the 3DS.
 *
 * Vertices arrive in clip space already (gfx_backend.h); the vertex shader only fits them to the top screen.
 * The RDP colour combiner is compiled into TEV stages, see compile_combiner().
 */
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx_backend.h"
#include "plat/plat.h"
#include "plat/profile.h"
#include "plat/settings.h"
#include "shader_shbin.h"

/* The display transfer converts to the screen's format and, with anti-aliasing, averages each 2x2 block of pixels. */
#define DISPLAY_TRANSFER_FLAGS(scaling)                                                                                \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                                 \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |                    \
     GX_TRANSFER_SCALING(scaling))

#define TOP_WIDTH 400
#define TOP_HEIGHT 240
/* Bytes of vertices per frame. Each of the two frame slots has its own buffer (see FrameSlot); races draw at most
 * about 3000 triangles, 9000 vertices, a frame, which is 48K of the largest vertices. */
#define VBO_BYTES (48 * 1024 * 48)

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
enum { VFMT_SHORT, VFMT_LONG, VFMT_OLD, VFMT_COUNT };
static const int sVfmtStride[VFMT_COUNT] = { sizeof(C3DVertex), sizeof(C3DVertex2), sizeof(C3DVertexOld) };
static C3D_AttrInfo sVfmtAttr[VFMT_COUNT];
static int sVfmtOld;       // DKR_VERTEX_FORMAT=48
static int sVfmtAttrSet;   // The format of citro3d's attribute setting
static int sVfmtBound = -1; // The format the vertex buffer setting reads, -1 for none yet this frame
static int sVfmtBase;      // Where in sVbo (bytes) the vertex buffer setting starts
static int sVfmtSwitches;   // Format changes in the frame

typedef struct C3DTexture {
    C3D_Tex tex;
    float scaleU, scaleV;
    int resample;
    int shared; // tex's memory belongs to another texture (gfxb_texture_share)
    int levels; // Mipmap levels after the first that tex holds (see mip_levels)
    int point;  // Point sampled, so the levels are never used
    int tinted; // The levels are tinted (the "Show" mipmap setting)
} C3DTexture;

static C3D_RenderTarget *sTarget;
/* Render target size relative to the screen: 2 draws at twice the resolution and downsamples (anti-aliasing). */
static int sRenderScale = 1;
static int sDefaultScale = 2;          // The setting (settings.txt, else DKR_AA).
static volatile int sWantedScale = 2;  // plat_set_antialias, applied by the renderer between frames.
static DVLB_s *sShaderDvlb;
static shaderProgram_s sProgram;
static u8 *sVbo; // The vertex buffer of the frame being built
static int sVboUsed; // Bytes
/* Vertices before this one have been flushed from the CPU cache this frame (see flush_new_vertices). */
static int sVboFlushed;
static C3D_Tex sWhiteTex;
extern u32 __ctru_linear_heap, __ctru_linear_heap_size;
static int sFrameStarted;
static int sDepthOnly;
static int sClearing; // draw_clear_rect is drawing
static int sFrameFirstDraw; // No draw yet in this frame's command list
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

/* The N64 picture is 320 pixels wide; by default it is shown centred on the 400 pixel wide top screen. */
static float sScreenScaleX = 320.0f / 400.0f;

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

static void flush_new_vertices(void) {
    if (sVboFlushed > sVboUsed) {
        sVboFlushed = 0; // The buffer was started again.
    }
    if (sVboUsed > sVboFlushed) {
        GSPGPU_FlushDataCache(sVbo + sVboFlushed, sVboUsed - sVboFlushed);
    }
    sVboFlushed = sVboUsed;
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
    u8 decal;
    u8 cull;
    short scissor[4];
} sGpu;

static void forget_gpu_state(void) {
    sGpu.known = 0;
}

/******************************** Setup ********************************/

/*
 * Vertices arrive in clip space for the N64 screen. To fit them to the 3DS top screen (in gfxb_draw_triangles):
 *  - x is scaled into the middle of the screen,
 *  - z is remapped from [-w, w] (near to far) to [-w, 0] with depth = -z / w, so nearer is larger; the N64 has no far
 *    plane, so anything beyond it is pinned to the farthest depth instead of being clipped,
 *  - then everything is rotated a quarter turn for the 3DS screen: x' = y, y' = -x.
 */
static inline void to_screen(const GfxVertex *v, float *pos) {
    float z = 0.5f * (v->z - v->w);
    pos[0] = v->y;
    pos[1] = -v->x * sScreenScaleX;
    pos[2] = z < 0.0f ? z : 0.0f;
    pos[3] = v->w;
}

/* The attribute settings of the vertex formats. The shader's inputs are v0 position, v1 and v2 the texture
 * coordinates, v3 colour; the new formats list them in the same order so that only the last one differs. */
static void init_vertex_formats(void) {
    C3D_AttrInfo *attr;
    int i;
    sVfmtOld = plat_getenv("DKR_VERTEX_FORMAT") != NULL && atoi(plat_getenv("DKR_VERTEX_FORMAT")) == 48;
    for (i = 0; i < VFMT_COUNT; i++) {
        attr = &sVfmtAttr[i];
        AttrInfo_Init(attr);
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
    if (!sVfmtOld) {
        C3D_FixedAttribSet(3, 0.0f, 0.0f, 0.0f, 0.0f); // Attribute 3 of the short format: v2
    }
    plat_log("vertex format: %s", sVfmtOld ? "48 bytes (DKR_VERTEX_FORMAT=48)" : "28 bytes, 36 with a second texture");
}

/*
 * Makes the GPU read the vertices from sVboUsed on in format fmt, and returns the index of the first. A change of
 * format starts the buffer setting at sVboUsed (rounded up to 16 bytes) with the new stride; the same format carries
 * on from the last draw's vertices, so the settings stay as they are.
 */
static int use_vertex_format(int fmt) {
    int stride = sVfmtStride[fmt];
    if (fmt != sVfmtBound) {
        C3D_BufInfo *buf = C3D_GetBufInfo();
        sVboUsed = (sVboUsed + 15) & ~15;
        sVfmtBase = sVboUsed;
        if (fmt != sVfmtAttrSet) {
            C3D_SetAttrInfo(&sVfmtAttr[fmt]);
            sVfmtAttrSet = fmt;
        }
        BufInfo_Init(buf);
        BufInfo_Add(buf, sVbo + sVfmtBase, stride, fmt == VFMT_SHORT ? 3 : 4, fmt == VFMT_SHORT ? 0x210 : 0x3210);
        if (sVfmtBound >= 0) {
            sVfmtSwitches++;
        }
        sVfmtBound = fmt;
    }
    return (sVboUsed - sVfmtBase) / stride;
}

/*
 * GPU self test, for finding what hangs a real 3DS when an emulator copes. Before the game starts, a few frames are
 * drawn that each add one feature of the renderer to the one before, and the bottom screen says which one never
 * finished. Once it has passed, gpu_test.txt in the data directory remembers that and later starts skip it, until
 * GPU_SELF_TEST_VERSION changes. DKR_GPU_TEST=1 runs it anyway and DKR_NO_GPU_TEST=1 never runs it.
 */

// Bump when the self test, the vertex format or the shader changes, so that the test runs again once.
#define GPU_SELF_TEST_VERSION 2
static void draw_clear_rect(const short rect[4], u32 rgba, int depthOnly);
static void apply_combiner(const GfxDrawState *state);
static void apply_fog(const GfxDrawState *state);

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
    C3D_RenderTargetClear(sTarget, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(sTarget);
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

static void gpu_self_test(void) {
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
    sVfmtBound = -1;

    plat_status("GPU test passed");
    gpu_test_mark_passed();
}

/* The render target for the top screen. The doubled one needs 3MB of VRAM, so fall back if it does not fit. */
static void create_target(int scale) {
    sTarget = NULL;
    if (scale == 2) {
        sTarget = C3D_RenderTargetCreate(TOP_HEIGHT * 2, TOP_WIDTH * 2, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
        sRenderScale = 2;
    }
    if (sTarget == NULL) {
        sTarget = C3D_RenderTargetCreate(TOP_HEIGHT, TOP_WIDTH, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
        sRenderScale = 1;
    }
    C3D_RenderTargetSetOutput(sTarget, GFX_TOP, GFX_LEFT,
                              DISPLAY_TRANSFER_FLAGS(sRenderScale == 2 ? GX_TRANSFER_SCALE_XY : GX_TRANSFER_SCALE_NO));
}

/* From any thread (the AA benchmark): anti-aliasing on (1), off (0) or back to the setting (-1), from the next
 * frame. */
void plat_set_antialias(int on) {
    sWantedScale = on < 0 ? sDefaultScale : on ? 2 : 1;
}

/* The Settings menu: the player's anti-aliasing choice, saved, and used from the next frame. */
void plat_3ds_set_antialias_setting(int on) {
    sDefaultScale = on ? 2 : 1;
    sWantedScale = sDefaultScale;
    settings_set("aa", on != 0);
}

int plat_3ds_antialias_setting(void) {
    return sDefaultScale == 2;
}

/* The Settings menu: the colour boost level (0 off, 1 to 3 stronger; see add_colour_boost), from the next frame. */
void plat_3ds_set_colour_setting(int level) {
    sColourSetting = level < 0 ? 0 : level > 3 ? 3 : level;
    settings_set("colour", sColourSetting);
}

int plat_3ds_colour_setting(void) {
    return sColourSetting;
}

/* Whether the frames being drawn now are anti-aliased. */
int plat_antialias(void) {
    return sRenderScale == 2;
}

/*
 * Renderer thread, between frames: makes a new render target if the anti-aliasing setting changed. A target can only
 * be deleted outside a frame with the GPU idle, so this waits for the GPU (an empty frame) first. The new target is
 * cleared like the very first one.
 */
static void apply_render_scale(void) {
    int scale = sWantedScale;
    if (scale == sRenderScale || sTarget == NULL) {
        return;
    }
    while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        plat_3ds_quit_point(PLAT_3DS_PARKED_GPU_BUSY);
        plat_sleep_ns(250000);
    }
    C3D_FrameEnd(0);
    C3D_RenderTargetDelete(sTarget); // Waits for the empty frame.
    create_target(scale);
    sFrameStarted = 0;
    plat_log("render target now %dx (anti-aliasing %s)", sRenderScale, sRenderScale == 2 ? "on" : "off");
    if (sRenderScale != scale) {
        sWantedScale = sRenderScale; // Didn't fit: don't try again every frame.
    }
}

static void alloc_frame_slots(void);

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
        // Anti-aliasing is the Settings menu's choice; until one is made, on unless DKR_AA=0.
        const char *aa = plat_getenv("DKR_AA");
        sDefaultScale = settings_get("aa", aa == NULL || atoi(aa) != 0) ? 2 : 1;
        sWantedScale = sDefaultScale;
        create_target(sDefaultScale);
        plat_status("Graphics started%s", sRenderScale == 2 ? " (anti-aliased)" : "");
    }
    sMipmapSetting = settings_get("mipmaps", 1);
    if (sMipmapSetting < 0 || sMipmapSetting > 2) {
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

    init_vertex_formats();
    alloc_frame_slots();

    for (i = 0; i < 64; i++) {
        white[i] = 0xFFFFFFFF;
    }
    C3D_TexInit(&sWhiteTex, 8, 8, GPU_RGBA8);
    C3D_TexUpload(&sWhiteTex, white);
    GSPGPU_FlushDataCache(sWhiteTex.data, 8 * 8 * 4);

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

void gfxb_capture_frame(const char *path) {
    snprintf(sCapturePath, sizeof(sCapturePath), "%s", path);
}

static void capture_pending_frame(void) {
    // The framebuffer is rotated (240 wide, 400 tall, times the render scale). It is copied at full resolution; the
    // comparison tool downsamples anti-aliased captures itself.
    const u32 w = TOP_HEIGHT * sRenderScale, h = TOP_WIDTH * sRenderScale;
    u32 *pixels;
    FILE *file;

    if (sCapturePath[0] == '\0') {
        return;
    }
    pixels = linearAlloc(w * h * 4);
    if (pixels != NULL) {
        GSPGPU_FlushDataCache(pixels, w * h * 4);
        C3D_SyncDisplayTransfer((u32 *) sTarget->frameBuf.colorBuf, GX_BUFFER_DIM(w, h), pixels, GX_BUFFER_DIM(w, h),
                                GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                    GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
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

static void remember_texture(const C3DTexture *t) {
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
static void check_textures_after_frame(void) {
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
static void log_buffers(void) {
    u16 w, h;
    u8 *top0, *top1, *bottom;
    top0 = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
    plat_log("buffers: command list %p (%lu bytes), vertices %p (%lu bytes), white texture %p, colour %p, depth %p",
             (void *) gpuCmdBuf, (unsigned long) gpuCmdBufSize * 4, (void *) sVbo,
             (unsigned long) VBO_BYTES, sWhiteTex.data, sTarget->frameBuf.colorBuf,
             sTarget->frameBuf.depthBuf);
    top1 = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, &w, &h);
    bottom = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
    plat_log("buffers: top screen %p / %p (%ux%u), bottom screen %p, linear heap %p (%lu bytes), %lu free",
             (void *) top0, (void *) top1, w, h, (void *) bottom, (void *) (uintptr_t) __ctru_linear_heap,
             (unsigned long) __ctru_linear_heap_size, (unsigned long) linearSpaceFree());
}

static void save_command_list(void) {
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
    u16 triangles;
    u8 blend, alphaTest, alphaRef, depthTest, depthWrite, decal, fog, cull, cycles;
    u8 cc[2][8]; // Per cycle: rgb a, b, c, d, then alpha a, b, c, d
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
} FrameSlot;
static FrameSlot sSlots[2];
static int sNumSlots = 1; // 2 unless the second slot didn't fit
static int sRec;          // The slot being built
static int sOnGpu = -1;   // The slot last handed to the GPU
static u32 sCmdWords;
static int sOverlap;      // This frame is built while the GPU draws the last one

/* The start of citro3d's context, which is not in its public header: the GPU command queue comes first. */
typedef struct C3DContextStart {
    gxCmdQueue_s gxQueue;
    u32 *cmdBuf;
    size_t cmdBufSize;
} C3DContextStart;
extern C3DContextStart __C3D_Context;
bool C3Di_SplitFrame(u32 **pBuf, u32 *pSize);

static u32 cmd_position(void) {
    return (u32) (gpuCmdBuf + gpuCmdBufOffset - sSlots[sRec].cmd);
}

/* Two frame slots, each with a vertex buffer and a command list as large as citro3d's own. */
static void alloc_frame_slots(void) {
    int i;
    sCmdWords = __C3D_Context.cmdBufSize;
    for (i = 0; i < 2; i++) {
        sSlots[i].vbo = linearAlloc(VBO_BYTES);
        sSlots[i].cmd = linearAlloc(sCmdWords * 4);
        if (sSlots[i].vbo == NULL || sSlots[i].cmd == NULL) {
            if (sSlots[i].vbo != NULL) {
                linearFree(sSlots[i].vbo);
            }
            if (sSlots[i].cmd != NULL) {
                linearFree(sSlots[i].cmd);
            }
            sSlots[i].vbo = NULL;
            sSlots[i].cmd = NULL;
            break;
        }
    }
    sNumSlots = i > 0 ? i : 1; // None can't happen this early, and nothing could be drawn.
    plat_log("frame slots: %d, each %lu KB of vertices and %lu KB of commands in linear memory, %lu KB left",
             sNumSlots, (unsigned long) (VBO_BYTES / 1024),
             (unsigned long) (sCmdWords * 4 / 1024), (unsigned long) (linearSpaceFree() / 1024));
    sVbo = sSlots[0].vbo;
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

static HangDraw *hang_draw_start(const GfxDrawState *state, int numTriangles) {
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
    d->blend = state->blend;
    d->alphaTest = state->alphaTest;
    d->alphaRef = state->alphaRef;
    d->depthTest = state->depthTest;
    d->depthWrite = state->depthWrite;
    d->decal = state->decal;
    d->fog = state->fogEnabled;
    d->cull = state->cullMode;
    d->cycles = state->combiner.numCycles;
    for (i = 0; i < 2; i++) {
        const GfxCCEquation *rgb = &state->combiner.rgb[i], *alpha = &state->combiner.alpha[i];
        const u8 cc[8] = { rgb->a, rgb->b, rgb->c, rgb->d, alpha->a, alpha->b, alpha->c, alpha->d };
        memcpy(d->cc[i], cc, sizeof(cc));
        d->tex[i] = (state->textures[i] != NULL && state->textures[i]->backendData != NULL)
                        ? &((const C3DTexture *) state->textures[i]->backendData)->tex
                        : NULL;
    }
    return d;
}

/* Writes gpu_hang.txt and gpu_hang_cmd.bin, once, when the GPU stops responding during the frame last handed over. */
static void save_gpu_hang(void) {
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
        sLastFrameTriangles, sRenderScale == 2 ? "on" : "off",
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
        int inStuck = stuckEntry >= 0 && d->cmdEnd > listStart && d->cmdStart < listEnd;
        int t;
        ADD("%c%4d cmd %5lu-%5lu tris %3u blend %u at %u/%u z %u/%u dec %u fog %u cull %u cyc %u", inStuck ? '*' : ' ',
            i, (unsigned long) d->cmdStart, (unsigned long) d->cmdEnd, d->triangles, d->blend, d->alphaTest,
            d->alphaRef, d->depthTest, d->depthWrite, d->decal, d->fog, d->cull, d->cycles);
        for (t = 0; t < d->cycles && t < 2; t++) {
            const u8 *c = d->cc[t];
            ADD(" cc %u,%u,%u,%u/%u,%u,%u,%u", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
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
void gfxb_log_draws(int snapshotFrame) {
    sLogFrame = sFrameCount;
    sSnapshotFrame = snapshotFrame;
    sSnapshotPending = snapshotFrame >= 0;
    if (sSnapshotPending) {
        log_buffers();
    }
}

/* Called on the main thread when the application closes, once the renderer has stopped with the GPU idle. */
void gfxb_3ds_shutdown(void) {
    C3D_Fini();
}

/* Deletes the textures freed while the slot's frame was built, once the GPU has drawn it. */
static void process_pending_frees(FrameSlot *slot) {
    int i;
    for (i = 0; i < slot->numFrees; i++) {
        if (!slot->frees[i]->shared) {
            C3D_TexDelete(&slot->frees[i]->tex);
        }
        free(slot->frees[i]);
    }
    slot->numFrees = 0;
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

void gfxb_start_frame(void) {
    FrameSlot *slot;
    // No vblank throttling here: the emulated VI already paces the game.
    capture_pending_frame(); // Outside a frame, so the copy waits for the GPU to finish the previous one.
    apply_render_scale();
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
    sVfmtBound = -1; // The first draw points the GPU at this slot's buffer.
    sVfmtSwitches = 0;
    GPUCMD_SetBuffer(slot->cmd, sCmdWords, 0);
    // Like an N64 framebuffer, the colour buffer keeps the last image: the game clears it itself, and a frame that
    // draws nothing (while a level loads) must not show up as a black flash. So only the first frame uses a memory
    // fill; after that the depth buffer is cleared by drawing. (A memory fill of the depth buffer alone never finished
    // on a real New 3DS, which hung the game after its first frame. Emulators complete it.)
    if (sFrameStarted >= 0) {
        C3D_RenderTargetClear(sTarget, C3D_CLEAR_ALL, 0x000000FF, 0);
    }
    // What C3D_FrameDrawOn does, which needs a citro3d frame begun; the target is marked used when handed over.
    C3D_SetFrameBuf(&sTarget->frameBuf);
    C3D_SetViewport(0, 0, sTarget->frameBuf.width, sTarget->frameBuf.height);
    forget_gpu_state();
    sMipmapsUsed = sMipmapSetting != 0;
    sMipmapsShown = sMipmapSetting == 2;
    sColourBoost = sColourSetting;
    sFrameFirstDraw = 1;
    if (sFrameStarted < 0) {
        static const short sFullScreen[4] = { 0, 0, GFX_SCREEN_WIDTH, GFX_SCREEN_HEIGHT };
        draw_clear_rect(sFullScreen, 0, 1);
    }
    sFrameStarted = 1;
}

/* DKR_GFX_TEST draws a reference quad: a 16x16 texture, red on top, blue at the bottom, with a green left column. */
static int sTestMode;
static void draw_test_pattern(void) {
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
    {
        static float sMaxUsage;
        FrameSlot *slot = &sSlots[sRec];
        u32 *tail = gpuCmdBuf, tailSize = gpuCmdBufSize, tailOffset = gpuCmdBufOffset;
        float usage;
        int i;

        if (full_flush()) {
            GSPGPU_FlushDataCache(sVbo, sVboUsed);
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
        if (sOverlap) {
            // Built while the GPU drew the last frame: wait for it now, then hand this one over.
            wait_for_gpu(1);
        }
        // C3D_FrameBegin set citro3d's own command buffer: carry on in the slot's, where the frame is.
        GPUCMD_SetBuffer(tail, tailSize, tailOffset);
        for (i = 0; i < slot->numParts; i++) {
            GX_ProcessCommandList(slot->partAddr[i], slot->partWords[i] * 4, GX_CMDLIST_FLUSH);
        }
        sTarget->used = true;
        sLastFrameTriangles = sFrameTriangles;
        sLastFrameUsage = usage;
        sFrameTriangles = 0;
        sLastDrainSplits = sDrainSplits;
        sDrainSplits = 0;
        sOnGpu = sRec;
        sRec = (sRec + 1) % sNumSlots;
        if (full_flush()) {
            C3D_FrameEnd(0); // Also flushes the whole linear heap.
        } else {
            C3D_FrameEnd(GX_CMDLIST_FLUSH);
        }
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

/* Sets the sampler: point or bilinear, with the mipmap levels in use or not. */
static void set_texture_filter(C3DTexture *t, int point) {
    if (point) {
        C3D_TexSetFilter(&t->tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetFilterMipmap(&t->tex, GPU_NEAREST);
    } else {
        C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetFilterMipmap(&t->tex, mip_filter());
    }
    t->point = point;
    t->tex.maxLevel = (sMipmapsUsed && !point) ? t->levels : 0;
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
static int upload_mip_levels(C3DTexture *t, const GfxTexture *tex, int format) {
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
        tex->backendData = t;
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
    set_texture_filter(t, tex->filter == GFX_FILTER_POINT);
    C3D_TexSetWrap(&t->tex, wrap_param(tex->wrapS), wrap_param(tex->wrapT));
    GSPGPU_FlushDataCache(data, C3D_TexCalcTotalSize(t->tex.size, t->levels));
    forget_gpu_state();
}

/* For snapshots: logs a texture whose memory no longer holds what was uploaded. */
static void verify_texture(const GfxTexture *tex) {
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
    FrameSlot *slot = &sSlots[sRec];
    if (t == NULL) {
        return;
    }
    tex->backendData = NULL;
    // The GPU may still be drawing with it, and this frame may have used it: delete it once the GPU has drawn this
    // frame (process_pending_frees).
    if (slot->numFrees == slot->freesCapacity) {
        int capacity = slot->freesCapacity ? slot->freesCapacity * 2 : 256;
        C3DTexture **list = realloc(slot->frees, capacity * sizeof(C3DTexture *));
        if (list == NULL) {
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
    set_texture_filter(t, dst->filter == GFX_FILTER_POINT);
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

static int fog_mode(void) {
    static int sMode = -1;
    if (sMode < 0) {
        sMode = plat_getenv("DKR_NO_FOG") != NULL ? FOG_OFF : plat_getenv("DKR_VERTEX_FOG") != NULL ? FOG_VERTEX : FOG_PIXEL;
    }
    return sMode;
}

static void apply_fog(const GfxDrawState *state) {
    static C3D_FogLut sLut;
    static int sLutMul = 0x7FFFFFFF, sLutOffset;
    static int sFogOn = -1;
    int on = state->fogEnabled && fog_mode() == FOG_PIXEL;

    if (on && (state->fogMul != sLutMul || state->fogOffset != sLutOffset)) {
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

static void compile_combiner(const GfxDrawState *state, TevProgram *prog, int boost) {
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
} CombinerInputs;

static void apply_combiner(const GfxDrawState *state) {
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
    if (sCompiled && memcmp(&in, &sLast, sizeof(in)) == 0) {
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
        if (!sGpu.known || memcmp(&stage, &sGpu.texEnv[i], sizeof(stage)) != 0) {
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

/* Returns whether the texture changed. */
static int bind_texture(int unit, GfxTexture *tex, float *scaleU, float *scaleV) {
    C3DTexture *t = (tex != NULL) ? (C3DTexture *) tex->backendData : NULL;
    C3D_Tex *bind = (t != NULL) ? &t->tex : &sWhiteTex;
    int changed = !sGpu.known || bind != sGpu.tex[unit];
    if (t != NULL && t->levels > 0 && t->tinted != sMipmapsShown && tex->pixels != NULL) {
        // The Show setting changed since the levels were built. The GPU may still be drawing the last frame with
        // them, which can show one frame of mixed levels.
        if (upload_mip_levels(t, tex, gpu_format(tex))) {
            GSPGPU_FlushDataCache(t->tex.data, C3D_TexCalcTotalSize(t->tex.size, t->levels));
        }
        t->tinted = sMipmapsShown;
    }
    if (t != NULL) {
        int maxLevel = (sMipmapsUsed && !t->point) ? t->levels : 0;
        if (t->tex.maxLevel != maxLevel) {
            t->tex.maxLevel = maxLevel; // The mipmap setting changed since it was set.
            changed = 1;
        }
    }
    if (changed) {
        C3D_TexBind(unit, bind);
        sGpu.tex[unit] = bind;
    }
    *scaleU = (t != NULL) ? t->scaleU : 1.0f;
    *scaleV = (t != NULL) ? t->scaleV : 1.0f;
    return changed;
}

/* Converts a scissor rectangle on the N64 screen to the rotated framebuffer. */
static void set_scissor(const short rect[4]) {
    float x0 = TOP_WIDTH / 2 + (rect[0] - GFX_SCREEN_WIDTH / 2) * sScreenScaleX * (400.0f / 320.0f);
    float x1 = TOP_WIDTH / 2 + (rect[2] - GFX_SCREEN_WIDTH / 2) * sScreenScaleX * (400.0f / 320.0f);
    int left, right, top, bottom;
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
    C3D_SetScissor(GPU_SCISSOR_NORMAL, left * sRenderScale, top * sRenderScale, right * sRenderScale,
                   bottom * sRenderScale);
}

/*
 * Snapshots on a real New 3DS showed small blended draws (a kart shadow, one letter of text) drawn as if their
 * texture were fully opaque, or their blending off: the shadow came out as a solid black square, the letter as a white
 * box, although the command list set exactly the same state as for the same draws that came out right. Each time the
 * draw before was a large opaque one, so the GPU looks to still be busy with it while the next draw's settings are
 * written. Flushing the framebuffer between them (DKR_FB_FLUSH=1, or 2 for every draw) did not help.
 *
 * DKR_GPU_SPLIT ends the command list before such a draw and starts a new one, which the GPU only begins once it has
 * finished the previous list: 1 (the default) before a blended draw that follows an unblended one, 2 whenever the
 * blending, alpha test or depth settings change, 0 never. This fixed the letter but not the shadows, which come later
 * in a race frame than the first 12 splits allowed. The GPU's command queue holds 32 entries a frame and a frame needs
 * 3 of its own, so at most MAX_DRAIN_SPLITS are made; past SMALL_DRAWS_ONLY, only for draws of a few triangles, which
 * are the ones that showed the problem.
 */
#define MAX_DRAIN_SPLITS 24
#define SMALL_DRAWS_ONLY 16
#define IS_BLENDED(blend) ((blend) == GFX_BLEND_ALPHA || (blend) == GFX_BLEND_ADD)

/*
 * The kart shadow still came out as a black square in the hub with the split above. Testing on a New 3DS showed that
 * writing the settings of the first blended draw twice fixes it (an empty triangle before it only sometimes did): the
 * GPU seems to miss some of the settings written right after an unblended draw. Both the split and the second write
 * are done.
 */

/*
 * The log behind the character select screen came and went from frame to frame on a New 3DS (issue #25). It is the
 * first draw after the screen clears, which are full screen draws with fog, depth and culling off, and the sky drawn
 * after it would cover it wherever the GPU drew it without its depth settings. So the draw after a clear rectangle is treated like
 * the first blended draw above: the command list is split before it and its settings are written twice.
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

/* Returns whether the draw's settings are to be written twice: a draw whose blending or depth settings differ from
 * the draw before, or the first draw of a frame. */
static int drain_before_draw(const GfxDrawState *state, int numTriangles) {
    static int sFlushMode = -1, sSplitMode = -1, sAfterClear = -1, sRewrite = -1;
    static unsigned int sLastKey = ~0u;
    unsigned int key;
    int split, first;

    if (sFlushMode < 0) {
        const char *env = plat_getenv("DKR_FB_FLUSH");
        sFlushMode = env != NULL ? atoi(env) : 0;
        env = plat_getenv("DKR_GPU_SPLIT");
        sSplitMode = env != NULL ? atoi(env) : 1;
        plat_log("framebuffer flush between draws: %s; command list split: %s",
                 sFlushMode == 0 ? "off" : sFlushMode == 1 ? "on state changes" : "always",
                 sSplitMode == 0 ? "off" : sSplitMode == 1 ? "before blending" : "on state changes");
        env = plat_getenv("DKR_AFTER_CLEAR");
        sAfterClear = env != NULL ? atoi(env) : 1;
        env = plat_getenv("DKR_REWRITE");
        sRewrite = env != NULL ? atoi(env) : 1;
        plat_log("settings written twice: %s", sRewrite ? "on every blend or depth change" : "known cases only");
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
static int apply_alpha_diagnostic(const GfxDrawState *state) {
    static int sOn = -1;
    int i;

    if (sOn < 0) {
        sOn = plat_getenv("DKR_DIAG_ALPHA") != NULL;
    }
    if (!sOn || !IS_BLENDED(state->blend)) {
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

static void set_draw_state(const GfxDrawState *state) {
    static int sNoCull = -1;
    u8 depthEnable, depthFunc, depthMask, cull;

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
    if (!sGpu.known || state->decal != sGpu.decal) {
        C3D_DepthMap(true, -1.0f, state->decal ? 0.0002f : 0.0f);
        sGpu.decal = state->decal;
    }
    if (sNoCull < 0) {
        sNoCull = plat_getenv("DKR_NO_CULL") != NULL;
    }
    cull = (sNoCull || state->cullMode == 0) ? GPU_CULL_NONE
           : state->cullMode == 1             ? GPU_CULL_BACK_CCW
                                              : GPU_CULL_FRONT_CCW;
    if (!sGpu.known || cull != sGpu.cull) {
        C3D_CullFace(cull);
        sGpu.cull = cull;
    }
    if (!sGpu.known || memcmp(state->scissor, sGpu.scissor, sizeof(sGpu.scissor)) != 0) {
        set_scissor(state->scissor);
        memcpy(sGpu.scissor, state->scissor, sizeof(sGpu.scissor));
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

/* Sets every GPU setting a draw uses. */
static void set_all_state(const GfxDrawState *state, float *su0, float *sv0, float *su1, float *sv1) {
    static int sCache = -1;
    int newTexture;

    if (sCache < 0) {
        const char *env = plat_getenv("DKR_STATE_CACHE");
        sCache = env != NULL ? atoi(env) : 1;
        plat_log("GPU settings: %s", sCache == 0   ? "all set for every draw"
                                     : sCache == 2 ? "only changes are set"
                                                   : "only changes are set, all draw settings with a new texture");
    }
    newTexture = bind_texture(0, state->textures[0], su0, sv0);
    newTexture |= bind_texture(1, state->textures[1], su1, sv1);
    apply_combiner(state);
    apply_fog(state);
    set_draw_state(state);
    if (newTexture && sGpu.known && sCache == 1) {
        C3D_CullFace((GPU_CULLMODE) sGpu.cull); // Same value: only marks the group for writing.
    }
    sGpu.known = sCache != 0;
    if (apply_alpha_diagnostic(state)) {
        // It wrote the combiner stages itself.
        forget_gpu_state();
    }
}

/* citro3d's own function that writes the changed settings to the command list (not in its public header). */
void C3Di_UpdateContext(void);

void gfxb_draw_triangles(const GfxDrawState *state, const GfxVertex *verts, int numTriangles) {
    HangDraw *hangDraw;
    float su0, sv0, su1, sv1;
    int i, first, vertex, count = numTriangles * 3;
    // Coordinates for a second texture only when the draw binds one (else it is the white texture).
    int fmt = sVfmtOld ? VFMT_OLD : state->textures[1] != NULL ? VFMT_LONG : VFMT_SHORT;

    if (sVboUsed + 15 + count * sVfmtStride[fmt] > VBO_BYTES || gpuCmdBufSize - gpuCmdBufOffset < sCmdWords / 16) {
        // Out of room in the frame's vertex buffer or command list (a full command list stops the program, and
        // nothing of the frame reaches the GPU before it ends, so splitting it would not make room): leave the rest
        // of the frame's triangles out. Races use under a fifth of either.
        if (sDrawsLeftOut++ < 5) {
            plat_log("frame %d: %s full (%d bytes of vertices, %lu command words), triangles left out", sFrameCount,
                     sVboUsed + 15 + count * sVfmtStride[fmt] > VBO_BYTES ? "vertex buffer" : "command list", sVboUsed,
                     (unsigned long) cmd_position());
        }
        return;
    }
    if (sLogFrame == -2) {
        const char *env = plat_getenv("DKR_LOG_DRAWS");
        sLogFrame = env != NULL ? atoi(env) : -1;
    }
    if (sFrameCount == sLogFrame) {
        const GfxCombiner *c = &state->combiner;
        if (state->textures[0] != NULL) {
            verify_texture(state->textures[0]);
            if (state->textures[0]->backendData != NULL) {
                remember_texture((const C3DTexture *) state->textures[0]->backendData);
            }
        }
        const GfxTexture *tex = state->textures[0];
        const C3DTexture *t = (tex != NULL) ? (const C3DTexture *) tex->backendData : NULL;
        plat_log("draw %d tris blend %d at %d/%d z %d/%d dec %d fog %d cull %d cc %d,%d,%d,%d/%d,%d,%d,%d cyc %d "
                 "tex %p (%dx%d gpu %dx%d at %p param %08lX) v0 %.3f %.3f %.3f %.3f uv %.3f %.3f rgba %d %d %d %d "
                 "cmd %lu",
                 numTriangles, state->blend, state->alphaTest, state->alphaRef, state->depthTest, state->depthWrite,
                 state->decal, state->fogEnabled, state->cullMode, c->rgb[0].a, c->rgb[0].b, c->rgb[0].c, c->rgb[0].d,
                 c->alpha[0].a, c->alpha[0].b, c->alpha[0].c, c->alpha[0].d, c->numCycles, (void *) tex,
                 tex != NULL ? tex->width : 0, tex != NULL ? tex->height : 0, t != NULL ? t->tex.width : 0,
                 t != NULL ? t->tex.height : 0, t != NULL ? t->tex.data : NULL,
                 (unsigned long) (t != NULL ? t->tex.param : 0), verts[0].x, verts[0].y, verts[0].z, verts[0].w, verts[0].u0,
                 verts[0].v0, verts[0].r, verts[0].g, verts[0].b, verts[0].a, (unsigned long) gpuCmdBufOffset);
    }
    hangDraw = hang_draw_start(state, numTriangles);
    first = drain_before_draw(state, numTriangles);
    if (first) {
        forget_gpu_state();
    }
    set_all_state(state, &su0, &sv0, &su1, &sv1);
    if (first) {
        // Write all the settings now, then mark them changed so that the draw writes them a second time.
        C3Di_UpdateContext();
        forget_gpu_state();
        set_all_state(state, &su0, &sv0, &su1, &sv1);
    }

    vertex = use_vertex_format(fmt);
    // Texture coordinates are scaled for padded textures, and t is flipped (the GPU's t = 0 is the last row).
    if (fmt == VFMT_SHORT) {
        C3DVertex *out = (C3DVertex *) (sVbo + sVboUsed);
        for (i = 0; i < count; i++) {
            const GfxVertex *v = &verts[i];
            to_screen(v, out[i].pos);
            out[i].tc0[0] = v->u0 * su0;
            out[i].tc0[1] = 1.0f - v->v0 * sv0;
            memcpy(out[i].color, &v->r, 4);
        }
    } else if (fmt == VFMT_LONG) {
        C3DVertex2 *out = (C3DVertex2 *) (sVbo + sVboUsed);
        for (i = 0; i < count; i++) {
            const GfxVertex *v = &verts[i];
            to_screen(v, out[i].pos);
            out[i].tc0[0] = v->u0 * su0;
            out[i].tc0[1] = 1.0f - v->v0 * sv0;
            memcpy(out[i].color, &v->r, 4);
            out[i].tc1[0] = v->u1 * su1;
            out[i].tc1[1] = 1.0f - v->v1 * sv1;
        }
    } else {
        C3DVertexOld *out = (C3DVertexOld *) (sVbo + sVboUsed);
        for (i = 0; i < count; i++) {
            const GfxVertex *v = &verts[i];
            to_screen(v, out[i].pos);
            out[i].tc0[0] = v->u0 * su0;
            out[i].tc0[1] = 1.0f - v->v0 * sv0;
            out[i].tc1[0] = v->u1 * su1;
            out[i].tc1[1] = 1.0f - v->v1 * sv1;
            out[i].color[0] = v->r;
            out[i].color[1] = v->g;
            out[i].color[2] = v->b;
            out[i].color[3] = v->a;
        }
    }
    C3D_DrawArrays(GPU_TRIANGLES, vertex, count);
    if (hangDraw != NULL) {
        hangDraw->cmdEnd = cmd_position();
    }
    sVboUsed += count * sVfmtStride[fmt];
    sFrameTriangles += numTriangles;
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
    sDepthOnly = 0;
    sClearing = 0;
}

void gfxb_clear_color(const short rect[4], unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    // Colour fills are drawn so that they cover only the game's part of the screen, leaving black bars at the sides.
    draw_clear_rect(rect, (r << 24) | (g << 16) | (b << 8) | a, 0);
}

void gfxb_clear_depth(const short rect[4]) {
    // Always drawn: a memory fill of the depth buffer alone hangs a real 3DS (see gfxb_start_frame).
    draw_clear_rect(rect, 0, 1);
}
