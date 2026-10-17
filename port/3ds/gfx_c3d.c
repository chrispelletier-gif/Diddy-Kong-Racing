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
#include "shader_shbin.h"

/* The display transfer converts to the screen's format and, with anti-aliasing, averages each 2x2 block of pixels. */
#define DISPLAY_TRANSFER_FLAGS(scaling)                                                                                \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                                 \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |                    \
     GX_TRANSFER_SCALING(scaling))

#define TOP_WIDTH 400
#define TOP_HEIGHT 240
#define VBO_VERTICES (3 * 24 * 1024)

/* What the vertex shader receives: positions already in the 3DS screen's clip space, colours from 0 to 1. */
typedef struct C3DVertex {
    float pos[4];
    float tc0[2];
    float tc1[2];
    float color[4];
} C3DVertex;

typedef struct C3DTexture {
    C3D_Tex tex;
    float scaleU, scaleV;
    int resample;
} C3DTexture;

static C3D_RenderTarget *sTarget;
/* Render target size relative to the screen: 2 draws at twice the resolution and downsamples (anti-aliasing). */
static int sRenderScale = 1;
static DVLB_s *sShaderDvlb;
static shaderProgram_s sProgram;
static C3DVertex *sVbo;
static int sVboUsed;
static C3D_Tex sWhiteTex;
static int sFrameStarted;
static int sDepthOnly;
static int sFrameCount, sFrameTriangles;
/* For the hang report: the size of the last frame handed to the GPU. */
static int sLastFrameTriangles;
static float sLastFrameUsage;
static int sCmdBufSplits;

/* The N64 picture is 320 pixels wide; by default it is shown centred on the 400 pixel wide top screen. */
static float sScreenScaleX = 320.0f / 400.0f;

/* Textures freed during a frame are deleted once the GPU is done with it. */
static C3DTexture **sPendingFrees;
static int sNumPendingFrees, sPendingFreesCapacity;

/******************************** Setup ********************************/

/*
 * Vertices arrive in clip space for the N64 screen. To fit them to the 3DS top screen (in gfxb_draw_triangles):
 *  - x is scaled into the middle of the screen,
 *  - z is remapped from [-w, w] (near to far) to [-w, 0] with depth = -z / w, so nearer is larger; the N64 has no far
 *    plane, so anything beyond it is pinned to the farthest depth instead of being clipped,
 *  - then everything is rotated a quarter turn for the 3DS screen: x' = y, y' = -x.
 */
static void to_screen(const GfxVertex *v, C3DVertex *out) {
    float z = 0.5f * (v->z - v->w);
    out->pos[0] = v->y;
    out->pos[1] = -v->x * sScreenScaleX;
    out->pos[2] = z < 0.0f ? z : 0.0f;
    out->pos[3] = v->w;
}

/*
 * GPU self test, for finding what hangs a real 3DS when an emulator copes. Before the game starts, a few frames are
 * drawn that each add one feature of the renderer to the one before, and the bottom screen says which one never
 * finished. Skipped with DKR_NO_GPU_TEST=1.
 */
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

static void gpu_test_triangle(C3DVertex *v) {
    static const float xs[3] = { -0.5f, 0.5f, 0.0f }, ys[3] = { -0.5f, -0.5f, 0.5f };
    int i;
    for (i = 0; i < 3; i++) {
        memset(&v[i], 0, sizeof(v[i]));
        v[i].pos[0] = xs[i];
        v[i].pos[1] = ys[i];
        v[i].pos[3] = 1.0f;
        v[i].color[0] = v[i].color[1] = v[i].color[2] = v[i].color[3] = 1.0f;
    }
}

static void gpu_test_step(const char *what) {
    sGpuTestStep++;
    plat_log("GPU test %d: %s", sGpuTestStep, what);
    gpu_test_wait(what);
    C3D_FrameBegin(0);
    C3D_RenderTargetClear(sTarget, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(sTarget);
}

static void gpu_test_end(const char *what) {
    C3D_FrameEnd(0);
    gpu_test_wait(what);
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
        "game clear rectangles",
    };
    GfxDrawState state;
    C3D_TexEnv *env;
    int i;

    if (plat_getenv("DKR_NO_GPU_TEST") != NULL) {
        return;
    }
    plat_status("GPU test...");
    gPlatGfxStage = "GPU test";

    // 1: an empty frame, like the game's first one.
    gpu_test_step(sNames[0]);
    gpu_test_end(sNames[0]);

    // 2: one flat triangle with the simplest state.
    gpu_test_step(sNames[1]);
    gpu_test_triangle(sVbo);
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
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    gpu_test_end(sNames[1]);

    // 3 to 8: add the renderer's features one at a time.
    gpu_test_step(sNames[2]);
    gpu_test_triangle(sVbo);
    C3D_TexBind(1, &sWhiteTex);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    gpu_test_end(sNames[2]);

    gpu_test_step(sNames[3]);
    gpu_test_triangle(sVbo);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    gpu_test_end(sNames[3]);

    gpu_test_step(sNames[4]);
    gpu_test_triangle(sVbo);
    C3D_SetScissor(GPU_SCISSOR_NORMAL, 40, 40, 200, 360);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    gpu_test_end(sNames[4]);

    gpu_test_step(sNames[5]);
    gpu_test_triangle(sVbo);
    C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL);
    C3D_DepthMap(true, -1.0f, 0.0002f);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
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
    gpu_test_triangle(sVbo);
    apply_combiner(&state);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    gpu_test_end(sNames[6]);

    gpu_test_step(sNames[7]);
    gpu_test_triangle(sVbo);
    state.fogEnabled = 1;
    state.fogMul = 256;
    state.fogOffset = -100;
    apply_fog(&state);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    state.fogEnabled = 0;
    apply_fog(&state);
    gpu_test_end(sNames[7]);

    gpu_test_step(sNames[8]);
    {
        static const short sFull[4] = { 0, 0, GFX_SCREEN_WIDTH, GFX_SCREEN_HEIGHT };
        sVboUsed = 0;
        draw_clear_rect(sFull, 0x204080FF, 0);
        draw_clear_rect(sFull, 0, 1);
        sVboUsed = 0;
        sFrameTriangles = 0;
    }
    gpu_test_end(sNames[8]);

    plat_status("GPU test passed");
}

void gfxb_init(void) {
    static int sDone;
    C3D_AttrInfo *attr;
    C3D_BufInfo *buf;
    u32 white[64];
    int i;

    // Called early from the main thread (plat_init), and again by the renderer thread.
    if (sDone) {
        return;
    }
    sDone = 1;
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2);
    {
        // Anti-aliasing is on unless DKR_AA=0. The doubled target needs 3MB of VRAM, so fall back if it does not fit.
        const char *aa = plat_getenv("DKR_AA");
        sTarget = NULL;
        if (aa == NULL || atoi(aa) != 0) {
            sTarget = C3D_RenderTargetCreate(TOP_HEIGHT * 2, TOP_WIDTH * 2, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
            sRenderScale = 2;
        }
        if (sTarget == NULL) {
            sTarget = C3D_RenderTargetCreate(TOP_HEIGHT, TOP_WIDTH, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
            sRenderScale = 1;
        }
        C3D_RenderTargetSetOutput(sTarget, GFX_TOP, GFX_LEFT,
                                  DISPLAY_TRANSFER_FLAGS(sRenderScale == 2 ? GX_TRANSFER_SCALE_XY : GX_TRANSFER_SCALE_NO));
        plat_status("Graphics started%s", sRenderScale == 2 ? " (anti-aliased)" : "");
    }

    sShaderDvlb = DVLB_ParseFile((u32 *) shader_shbin, shader_shbin_size);
    shaderProgramInit(&sProgram);
    shaderProgramSetVsh(&sProgram, &sShaderDvlb->DVLE[0]);
    C3D_BindProgram(&sProgram);

    attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 3, GPU_FLOAT, 4);

    sVbo = linearAlloc(sizeof(C3DVertex) * VBO_VERTICES);
    buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, sVbo, sizeof(C3DVertex), 4, 0x3210);

    for (i = 0; i < 64; i++) {
        white[i] = 0xFFFFFFFF;
    }
    C3D_TexInit(&sWhiteTex, 8, 8, GPU_RGBA8);
    C3D_TexUpload(&sWhiteTex, white);

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
    u32 hash;
} sChecked[MAX_CHECKED_TEXTURES];
static int sNumChecked;

static u32 texture_hash(const C3DTexture *t) {
    const u32 *data = (const u32 *) t->tex.data;
    u32 n = t->tex.width * t->tex.height, i, hash = 2166136261u;
    // Read the memory the GPU reads (flushing first, so nothing the CPU wrote is thrown away).
    GSPGPU_FlushDataCache(data, n * 4);
    GSPGPU_InvalidateDataCache(data, n * 4);
    for (i = 0; i < n; i++) {
        hash = (hash ^ data[i]) * 16777619u;
    }
    return hash;
}

static void remember_texture(const C3DTexture *t) {
    int i;
    for (i = 0; i < sNumChecked; i++) {
        if (sChecked[i].tex == t) {
            return;
        }
    }
    if (sNumChecked < MAX_CHECKED_TEXTURES) {
        sChecked[sNumChecked].tex = t;
        sChecked[sNumChecked].data = t->tex.data;
        sChecked[sNumChecked].hash = texture_hash(t);
        sNumChecked++;
    }
}

/* Once the GPU has finished the snapshot frame, and before any texture is deleted. */
static void check_textures_after_frame(void) {
    int i, changed = 0;
    for (i = 0; i < sNumChecked; i++) {
        const C3DTexture *t = sChecked[i].tex;
        if (t->tex.data != sChecked[i].data || texture_hash(t) != sChecked[i].hash) {
            plat_log("after the GPU: texture memory at %p (%dx%d) changed", sChecked[i].data, t->tex.width,
                     t->tex.height);
            changed++;
        }
    }
    plat_log("after the GPU: %d of %d textures changed", changed, sNumChecked);
    sNumChecked = 0;
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

/* Logs every draw of the current frame. */
void gfxb_log_draws(int snapshotFrame) {
    sLogFrame = sFrameCount;
    sSnapshotFrame = snapshotFrame;
    sNumChecked = 0;
}

/* Called on the main thread when the application closes, once the renderer has stopped with the GPU idle. */
void gfxb_3ds_shutdown(void) {
    C3D_Fini();
}

static void process_pending_frees(void) {
    int i;
    for (i = 0; i < sNumPendingFrees; i++) {
        C3D_TexDelete(&sPendingFrees[i]->tex);
        free(sPendingFrees[i]);
    }
    sNumPendingFrees = 0;
}

void gfxb_start_frame(void) {
    // No vblank throttling here: the emulated VI already paces the game. Beginning a frame still waits for the GPU
    // to finish the previous one, after which its textures can be deleted and the vertex buffer reused.
    capture_pending_frame(); // Outside a frame, so the copy waits for the GPU to finish the previous one.
    if (sFrameCount < 3) {
        plat_log("frame %d: waiting for the GPU", sFrameCount);
    }
    gPlatGfxStage = "waiting for the GPU";
    if (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        // Poll instead of blocking, so that a GPU that never finishes the previous frame gets reported.
        unsigned long long start = plat_time_ns();
        int reported = 0;
        while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
            if (!reported && plat_time_ns() - start > 3000000000ULL) {
                reported = 1;
                gPlatGfxStage = "GPU not responding";
                plat_status("GPU stuck after frame %d (%d triangles, cmd buffer %.0f%%)", sFrameCount - 1,
                            sLastFrameTriangles, sLastFrameUsage * 100.0f);
            }
            plat_3ds_quit_point(PLAT_3DS_PARKED_GPU_BUSY);
            plat_sleep_ns(250000);
        }
    }
    // The application is closing: stop here, with the GPU idle and no frame being built, so it can be shut down.
    plat_3ds_quit_point(PLAT_3DS_PARKED_GPU_IDLE);
    gPlatGfxStage = "drawing";
    if (sNumChecked > 0) {
        check_textures_after_frame();
    }
    if (sFrameCount < 3) {
        plat_log("frame %d: GPU ready", sFrameCount);
    }
    if (sFrameCount == 1) {
        plat_status("First frame drawn");
    }
    process_pending_frees();
    // Like an N64 framebuffer, the colour buffer keeps the last image: the game clears it itself, and a frame that
    // draws nothing (while a level loads) must not show up as a black flash. So only the first frame uses a memory
    // fill; after that the depth buffer is cleared by drawing. (A memory fill of the depth buffer alone never finished
    // on a real New 3DS, which hung the game after its first frame. Emulators complete it.)
    if (sFrameStarted >= 0) {
        C3D_RenderTargetClear(sTarget, C3D_CLEAR_ALL, 0x000000FF, 0);
    }
    C3D_FrameDrawOn(sTarget);
    sVboUsed = 0;
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
        float usage = C3D_GetCmdBufUsage();
        if (usage > sMaxUsage) {
            sMaxUsage = usage;
        }
        if ((sFrameCount++ % 120) == 0) {
            plat_log("frame %d: %d triangles at %llu ms, command buffer %.0f%% (max %.0f%%)", sFrameCount,
                     sFrameTriangles, plat_time_ns() / 1000000ULL, usage * 100.0f, sMaxUsage * 100.0f);
        }
    }
    sLastFrameTriangles = sFrameTriangles;
    sLastFrameUsage = C3D_GetCmdBufUsage();
    sFrameTriangles = 0;
    GSPGPU_FlushDataCache(sVbo, sVboUsed * sizeof(C3DVertex));
    C3D_FrameEnd(0);
    sFrameStarted = -1; // A frame has been drawn, so the target holds an image.
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
 * The GPU texel at (px, py) of the power of two sized texture. Wrapping textures that aren't a power of two are
 * stretched to fit, others are padded (repeating their edge texels) and their coordinates scaled.
 */
static u32 gpu_texel(const GfxTexture *tex, unsigned int pw, unsigned int ph, int resample, unsigned int px,
                     unsigned int py) {
    unsigned int w = tex->width, h = tex->height;
    unsigned int sx, sy;
    u32 c;
    if (resample) {
        sx = px * w / pw;
        sy = py * h / ph;
    } else {
        sx = (px < w) ? px : w - 1;
        sy = (py < h) ? py : h - 1;
    }
    c = tex->pixels[sy * w + sx];
    // RGBA8888 with R in the low byte -> the GPU's RGBA8 (R in the high byte).
    return ((c & 0xFF) << 24) | ((c & 0xFF00) << 8) | ((c >> 8) & 0xFF00) | (c >> 24);
}

void gfxb_texture_upload(GfxTexture *tex) {
    C3DTexture *t = (C3DTexture *) tex->backendData;
    unsigned int w = tex->width, h = tex->height;
    unsigned int pw = next_pot(w), ph = next_pot(h);
    int resample = (tex->wrapS != GFX_WRAP_CLAMP && pw != w) || (tex->wrapT != GFX_WRAP_CLAMP && ph != h);
    u32 *data;
    unsigned int tx, ty;
    int i;

    if (t == NULL) {
        t = calloc(1, sizeof(C3DTexture));
        if (t == NULL || !C3D_TexInit(&t->tex, pw, ph, GPU_RGBA8)) {
            static int sFailures;
            gPlatCounters[PLAT_COUNTER_TEXTURE_FAILED]++;
            if ((sFailures++ % 50) == 0) {
                plat_log("texture %ux%u allocation failed (%d failures, linear free %lu)", pw, ph, sFailures,
                         (unsigned long) linearSpaceFree());
            }
            free(t);
            return;
        }
        tex->backendData = t;
    }
    // Swizzled straight into the texture's memory in 8x8 tiles, top row first (the GPU samples t = 0 from the start
    // of the data). No temporary buffer: when the heap was short, skipping the upload left whatever the memory held
    // before, such as an old solid black texture. The data reaches the GPU with the frame's cache flush.
    data = (u32 *) t->tex.data;
    for (ty = 0; ty < ph; ty += 8) {
        for (tx = 0; tx < pw; tx += 8) {
            u32 *tile = &data[(ty / 8) * (pw / 8) * 64 + (tx / 8) * 64];
            for (i = 0; i < 64; i++) {
                tile[sTileOrder[i]] = gpu_texel(tex, pw, ph, resample, tx + (i & 7), ty + (i >> 3));
            }
        }
    }
    t->resample = resample;
    t->scaleU = resample ? 1.0f : (float) w / pw;
    t->scaleV = resample ? 1.0f : (float) h / ph;
    if (tex->filter == GFX_FILTER_POINT) {
        C3D_TexSetFilter(&t->tex, GPU_NEAREST, GPU_NEAREST);
    } else {
        C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_LINEAR);
    }
    C3D_TexSetWrap(&t->tex, wrap_param(tex->wrapS), wrap_param(tex->wrapT));
}

/* For snapshots: logs a texture whose memory no longer holds what was uploaded. */
static void verify_texture(const GfxTexture *tex) {
    const C3DTexture *t = (const C3DTexture *) tex->backendData;
    unsigned int pw, ph, tx, ty, wrong = 0, firstAt = 0;
    u32 firstGot = 0, firstWant = 0;
    const u32 *data;
    int i;

    if (t == NULL) {
        plat_log("  texture %p %dx%d has no GPU copy", (const void *) tex, tex->width, tex->height);
        return;
    }
    pw = t->tex.width;
    ph = t->tex.height;
    data = (const u32 *) t->tex.data;
    // Read what is in memory, not what the CPU cache holds.
    GSPGPU_FlushDataCache(data, pw * ph * 4);
    GSPGPU_InvalidateDataCache(data, pw * ph * 4);
    for (ty = 0; ty < ph; ty += 8) {
        for (tx = 0; tx < pw; tx += 8) {
            const u32 *tile = &data[(ty / 8) * (pw / 8) * 64 + (tx / 8) * 64];
            for (i = 0; i < 64; i++) {
                u32 want = gpu_texel(tex, pw, ph, t->resample, tx + (i & 7), ty + (i >> 3));
                if (tile[sTileOrder[i]] != want) {
                    if (wrong++ == 0) {
                        firstAt = (ty + (i >> 3)) * pw + tx + (i & 7);
                        firstGot = tile[sTileOrder[i]];
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
    // The GPU may still be drawing with it: delete it once the next frame has begun.
    if (sNumPendingFrees == sPendingFreesCapacity) {
        int capacity = sPendingFreesCapacity ? sPendingFreesCapacity * 2 : 256;
        C3DTexture **list = realloc(sPendingFrees, capacity * sizeof(C3DTexture *));
        if (list == NULL) {
            C3D_TexDelete(&t->tex); // Out of memory: delete now and risk a glitch.
            free(t);
            return;
        }
        sPendingFrees = list;
        sPendingFreesCapacity = capacity;
    }
    sPendingFrees[sNumPendingFrees++] = t;
}

/******************************** Combiner ********************************/

/* What a stage's constant colour holds. */
typedef struct ConstSlot {
    int rgb;   // GFX_CC_* source in the RGB channels, or -1 if unused
    int alpha; // GFX_CC_* source in the alpha channel, or -1 if unused
} ConstSlot;

typedef struct TevStage {
    GPU_TEVSRC rgbSrc[3], alphaSrc[3];
    int rgbOp[3], alphaOp[3];
    GPU_COMBINEFUNC rgbFunc, alphaFunc;
    ConstSlot konst;
} TevStage;

typedef struct TevProgram {
    TevStage stages[6];
    int numStages;
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
        C3D_FogColor(state->fog[0] | (state->fog[1] << 8) | (state->fog[2] << 16));
    }
    if (on != sFogOn) {
        C3D_FogGasMode(on ? GPU_FOG : GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        sFogOn = on;
    }
}

static void compile_combiner(const GfxDrawState *state, TevProgram *prog) {
    static const GfxCCEquation passthrough = { GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_COMBINED };
    int cycle;

    prog->numStages = 0;
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

static void apply_combiner(const GfxDrawState *state) {
    TevProgram prog;
    int i;

    compile_combiner(state, &prog);
    for (i = 0; i < 6; i++) {
        C3D_TexEnv *env = C3D_GetTexEnv(i);
        C3D_TexEnvInit(env);
        if (i < prog.numStages) {
            const TevStage *st = &prog.stages[i];
            u32 r, g, b, a;
            C3D_TexEnvSrc(env, C3D_RGB, st->rgbSrc[0], st->rgbSrc[1], st->rgbSrc[2]);
            C3D_TexEnvSrc(env, C3D_Alpha, st->alphaSrc[0], st->alphaSrc[1], st->alphaSrc[2]);
            C3D_TexEnvOpRgb(env, st->rgbOp[0], st->rgbOp[1], st->rgbOp[2]);
            C3D_TexEnvOpAlpha(env, st->alphaOp[0], st->alphaOp[1], st->alphaOp[2]);
            C3D_TexEnvFunc(env, C3D_RGB, st->rgbFunc);
            C3D_TexEnvFunc(env, C3D_Alpha, st->alphaFunc);
            r = const_value(state, st->konst.rgb, 0);
            g = const_value(state, st->konst.rgb, 1);
            b = const_value(state, st->konst.rgb, 2);
            a = const_value(state, st->konst.alpha, 3);
            if (st->konst.alpha == GFX_CC_ONE || st->konst.alpha == GFX_CC_PRIM_LOD_FRACTION) {
                a = const_value(state, st->konst.alpha, 0);
            }
            C3D_TexEnvColor(env, r | (g << 8) | (b << 16) | (a << 24));
        } else {
            // Unused stages pass the previous stage through.
            C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        }
    }
}

/******************************** Drawing ********************************/

static void bind_texture(int unit, GfxTexture *tex, float *scaleU, float *scaleV) {
    C3DTexture *t = (tex != NULL) ? (C3DTexture *) tex->backendData : NULL;
    if (t != NULL) {
        C3D_TexBind(unit, &t->tex);
        *scaleU = t->scaleU;
        *scaleV = t->scaleV;
    } else {
        C3D_TexBind(unit, &sWhiteTex);
        *scaleU = 1.0f;
        *scaleV = 1.0f;
    }
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

void gfxb_draw_triangles(const GfxDrawState *state, const GfxVertex *verts, int numTriangles) {
    C3DVertex *out;
    float su0, sv0, su1, sv1;
    int i, count = numTriangles * 3;

    if (sVboUsed + count > VBO_VERTICES) {
        return;
    }
    if (gpuCmdBufOffset > gpuCmdBufSize - gpuCmdBufSize / 10) {
        // A full command buffer silently drops commands, and a half written command can hang the real GPU (an
        // emulator shrugs it off). Hand what is there to the GPU and carry on in an empty buffer instead.
        extern u32 __ctru_linear_heap, __ctru_linear_heap_size;
        if (sCmdBufSplits++ < 5) {
            plat_log("frame %d: command buffer full, splitting it", sFrameCount);
        }
        GSPGPU_FlushDataCache((void *) __ctru_linear_heap, __ctru_linear_heap_size);
        C3D_FrameSplit(GX_CMDLIST_FLUSH);
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
    bind_texture(0, state->textures[0], &su0, &sv0);
    bind_texture(1, state->textures[1], &su1, &sv1);
    apply_combiner(state);
    apply_fog(state);

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
    C3D_AlphaTest(state->alphaTest, GPU_GEQUAL, state->alphaRef);
    if (sDepthOnly) {
        C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_DEPTH);
    } else if (state->depthTest || state->depthWrite) {
        C3D_DepthTest(true, state->depthTest ? GPU_GEQUAL : GPU_ALWAYS,
                      state->depthWrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
    } else {
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    }
    C3D_DepthMap(true, -1.0f, state->decal ? 0.0002f : 0.0f);
    {
        static int sNoCull = -1;
        if (sNoCull < 0) {
            sNoCull = plat_getenv("DKR_NO_CULL") != NULL;
        }
        C3D_CullFace((sNoCull || state->cullMode == 0) ? GPU_CULL_NONE
                     : state->cullMode == 1                ? GPU_CULL_BACK_CCW
                                                           : GPU_CULL_FRONT_CCW);
    }
    set_scissor(state->scissor);

    out = &sVbo[sVboUsed];
    for (i = 0; i < count; i++) {
        const GfxVertex *v = &verts[i];
        to_screen(v, &out[i]);
        // Texture coordinates are scaled for padded textures, and t is flipped (the GPU's t = 0 is the last row).
        out[i].tc0[0] = v->u0 * su0;
        out[i].tc0[1] = 1.0f - v->v0 * sv0;
        out[i].tc1[0] = v->u1 * su1;
        out[i].tc1[1] = 1.0f - v->v1 * sv1;
        out[i].color[0] = v->r * (1.0f / 255.0f);
        out[i].color[1] = v->g * (1.0f / 255.0f);
        out[i].color[2] = v->b * (1.0f / 255.0f);
        out[i].color[3] = v->a * (1.0f / 255.0f);
    }
    C3D_DrawArrays(GPU_TRIANGLES, sVboUsed, count);
    sVboUsed += count;
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
    gfxb_draw_triangles(&state, v, 2);
    sDepthOnly = 0;
}

void gfxb_clear_color(const short rect[4], unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    // Colour fills are drawn so that they cover only the game's part of the screen, leaving black bars at the sides.
    draw_clear_rect(rect, (r << 24) | (g << 16) | (b << 8) | a, 0);
}

void gfxb_clear_depth(const short rect[4]) {
    // Always drawn: a memory fill of the depth buffer alone hangs a real 3DS (see gfxb_start_frame).
    draw_clear_rect(rect, 0, 1);
}
