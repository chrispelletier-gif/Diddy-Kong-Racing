/*
 * Display list interpreter for the F3DDKR microcode and the RDP.
 *
 * Vertices are transformed on the CPU (the game already supplies combined model-view-projection matrices),
 * textures are decoded from RDRAM into RGBA8888 and cached, and the RDP colour combiner and blender settings are
 * translated into a GfxDrawState for the backend (gfx_backend.h).
 */
#include <ultra64.h>
#include "types.h"
#include "f3ddkr.h"
#include "structs.h"
#include "port.h"
#include "plat/plat.h"
#include "plat/profile.h"
#include "gfx_backend.h"
#include "gfx_split.h"
#include "port_drawtag.h"

#define MAX_VERTICES 64
#define MAX_BATCH_TRIANGLES GFX_MAX_BATCH_TRIANGLES
#define MAX_BATCH_VERTICES GFX_MAX_BATCH_VERTICES
#define DL_STACK_SIZE 18

/******************************** State ********************************/

typedef struct RspVertex {
    float x, y, z, w; // Clip space, already including the viewport transform (see gfx_backend.h)
    union {
        struct {
            u8 r, g, b, a;
        };
        u32 rgba; // The four together, as GfxVertex holds them: copied as one word (draw_dkr_triangles)
    };
    union {
        struct {
            u8 fog;
            u8 fogValid; // The fog factor is only worked out while G_FOG is set (see load_vertices)
            u8 flat;     // Loaded with an orthographic matrix: drawn flat on the screen (1, or GFX_FLAT_PLACED for a sprite)
            u8 unused;
        };
        u32 fogFlat; // The same as one word, written at once by load_vertices (fog in the low byte)
    };
} RspVertex;

typedef struct Tile {
    u8 fmt, siz;
    u16 line; // In 64 bit words
    u16 tmem; // In 64 bit words
    u8 palette;
    u8 cms, cmt;
    u8 masks, maskt;
    u8 shifts, shiftt;
    u16 uls, ult, lrs, lrt; // 10.2 fixed point
} Tile;

/* What a texture load put into TMEM. Textures are decoded straight from RDRAM using this information. */
typedef struct TmemLoad {
    const u8 *addr;
    u32 size;       // Bytes loaded
    u32 rowStride;  // Bytes between rows in RDRAM (LoadTile), or 0 for LoadBlock
    u8 lineSwapped; // LoadBlock without dxt: odd rows are stored pre-swapped
    u8 valid;
} TmemLoad;

static struct {
    // RSP
    float matrices[4][4][4];
    s32 curMatrix;
    s32 billboard;
    RspVertex verts[MAX_VERTICES];
    s32 vertexIndex;
    s32 vertsWithoutFog; // Some vertices were loaded with G_FOG off, so they have no fog factor yet
    u64 flatVerts;       // Bit i: verts[i] may be flat (DKR_WALK_LEAN; a load with no flat vertices clears its bits)
    u32 geometryMode;
    float vpScale[3], vpTrans[3];
    s16 fogMul, fogOffset;

    // RDP
    u32 omH, omL;
    u32 combineW0, combineW1;
    GfxCombiner combiner;
    u8 combinerTextures[3]; // Indexed by the number of cycles: bit 0 if the combiner reads texel 0, bit 1 texel 1
    u8 prim[4], env[4], fogColor[4], blendColor[4];
    u8 primLodFrac;
    u32 fillColor;
    Tile tiles[8];
    struct {
        u8 fmt, siz;
        u16 width;
        const u8 *addr;
    } timg;
    TmemLoad tmem[512 / 8 + 1]; // Indexed by TMEM address in units of 8 words (64 bytes)
    u64 tmemValid;              // Bit i: tmem[i].valid, so that a load can forget the ones it covers at once
    const u8 *tlutAddr;
    u32 tlutCount;
    s32 tlutFirst; // The TLUT entry tlutAddr is (TMEM word - 256).
    const u8 *colorImage;
    const u8 *depthImage;
    s16 scissor[4];
    u32 rdpHalf1, rdpHalf2;
} g;

static s32 sTraceFrame = -1;
static s32 sTracing; // This frame is the traced one (DKR_TRACE_FRAME, or a snapshot): one flag for the walk to test

/* Code the walk rarely or never runs (logging, tracing, diagnostics) is kept out of its hot functions, so that
 * what runs every frame stays within the Old 3DS's 16 KB instruction cache (roadmap O9), and the per-draw functions
 * are HOT: laid out together on the 3DS (plat.h PLAT_HOT). */
#define COLD PLAT_COLD
#define NOINLINE PLAT_NOINLINE
#define HOT PLAT_HOT
#define UNLIKELY(x) __builtin_expect((x) != 0, 0)

/*
 * Differential testing (DKR_DIFF_EVERY=n, 3DS build): every nth frame is also drawn by the software reference renderer
 * (3ds/gfx_ref.c) and both results are saved to the data directory, as diff_NNNNN_ref.png and diff_NNNNN_gpu.bin,
 * for port/tools/diff_frames.py to compare.
 */
#ifdef PORT_GFX_REFERENCE
void refb_init(void);
void refb_start_frame(void);
void refb_end_frame(void);
void refb_clear_color(const short rect[4], unsigned char r, unsigned char g, unsigned char b, unsigned char a);
void refb_clear_depth(const short rect[4]);
void refb_draw_indexed(const GfxDrawState *state, const GfxVertex *verts, int numVerts, const unsigned short *indices,
                       int numTriangles);
void refb_save_png(const char *path);
void gfxb_capture_frame(const char *path);
void gfxb_log_draws(int snapshotFrame);
static s32 sDiffEvery;
static s32 sRefActive;
static s32 sRefInitialised;
#define REF_CALL(call)                                                                                                 \
    do {                                                                                                               \
        if (UNLIKELY(sRefActive)) {                                                                                    \
            call;                                                                                                      \
        }                                                                                                              \
    } while (0)
#else
#define REF_CALL(call)
#endif
#ifdef PORT_GFX_REFERENCE
#define REF_ACTIVE() sRefActive
#else
#define REF_ACTIVE() 0
#endif
#define TRACE(...)                        \
    if (UNLIKELY(sTracing)) {             \
        plat_log(__VA_ARGS__);            \
    }

/*
 * The batch of triangles waiting to be drawn, as vertices and indices into them (gfxb_draw_indexed). Corners that are
 * the same vertex drawn with the same texture coordinates share one entry (see draw_dkr_triangles), so the backend
 * converts each about once instead of three times a triangle, and the GPU transforms it once.
 */
static GfxVertex sBatchVerts[MAX_BATCH_VERTICES];
static u16 sBatchIndices[MAX_BATCH_TRIANGLES * 3];
static s32 sBatchCount;    // Triangles
static s32 sBatchVertices; // Vertices used
static GfxDrawState sBatchState;
/* The batch of model triangles (GPU vertex maths, P30), see sModelVerts, and the batch of mesh parts (P31, P34),
 * see mesh_part, which share the state and parameters: only one of the three batches holds triangles at a time. */
static s32 sModelCount;
static GfxDrawState sModelState;
static s32 sMeshPartCount;
/* Changes whenever the batch's vertices become invalid (a flush, or a new triangle command): entries of the
 * triangle command's shared vertex table are only valid for the generation they were made in. */
static u32 sBatchGeneration;

/*
 * The draw state of the last triangle command. Between two triangle commands DKR usually only loads vertices or
 * matrices, so the next one can reuse it instead of building it (and looking up its textures) again. Any command that
 * can change the draw state clears sTriStateValid (see sOpInfo).
 */
static GfxDrawState sTriState;
static s32 sTriStateValid;
static s32 sFrameCount;

/*
 * Where the display list walk's time goes while the profiler is on (P26; the profile.h PROF_PART_WALK parts). The
 * clock is read only when the walk moves from one part to another, which leaves runs of state commands untimed one by
 * one. Each span between two reads also holds about one read's own cost, which is taken back out of its part (and,
 * through PROF_PART_WALK_TIMER, out of the renderer's time). Draws and texture decodes are timed elsewhere, so their
 * parts here only keep them out of the others.
 *
 * On the 3DS a clock read is a system call (svcGetSystemTick), and the walk makes 1,400-1,900 of them in a race frame
 * (one per command that changes the part, two per texture lookup, draw state and draw): on an Old 3DS they are about
 * 2 ms of a frame as calibrated, and each one also evicts the renderer's code and data from the 16 KB caches, which
 * the calibration does not see. DKR_WALK_PARTS=0 (the Old 3DS's default) leaves the breakdown off: the walk then
 * reads nothing, and the renderer's time (PROF_RDP, one read at each end of the display list) still counts. The
 * reads sit out of line and cold, so the hot loop carries one load and branch per command.
 */
enum WalkPart {
    WALK_COMMANDS,
    WALK_MATRICES,
    WALK_VERTICES,
    WALK_TRIANGLES,
    WALK_RECTS,
    WALK_DRAWSTATE,
    WALK_TEXTURES,
    WALK_FRAMEEND,
    WALK_DRAWS,
    WALK_DECODE,
    WALK_PART_COUNT
};
#define WALK_REPORTED WALK_DRAWS // Parts before this one are reported, in the order of the PROF_PART_WALK ones

static s32 sWalkParts; // DKR_WALK_PARTS: time the walk's parts while the profiler is on (read_switches)
static s32 sWalkTimed; // This display list's parts are being timed (walk_begin)
static u32 sWalkPart;
static unsigned long long sWalkLast;
static unsigned long long sWalkTicks[WALK_PART_COUNT];
static u32 sWalkSpans[WALK_PART_COUNT];
static unsigned long long sWalkReadCost; // Ticks one clock read takes, in 1/256ths

/* The timed walk's switch to this part (walk_switch): the clock read and bookkeeping, out of line and cold, as only
 * the profiler takes them. Callers test for a change of part first, so that runs of commands in one part cost no call. */
static COLD u32 walk_switch_timed(u32 part) {
    u32 old = sWalkPart;
    if (part != old) {
        unsigned long long now = plat_ticks();
        sWalkTicks[old] += now - sWalkLast;
        sWalkSpans[old]++;
        sWalkLast = now;
        sWalkPart = part;
    }
    return old;
}

/* Switches the walk's timing to this part. Returns the part it was in, to switch back to (nothing while untimed). */
static inline u32 walk_switch(u32 part) {
    if (!sWalkTimed) {
        return 0;
    }
    return part != sWalkPart ? walk_switch_timed(part) : part;
}

static void walk_begin(void) {
    s32 i;
    sWalkTimed = sWalkParts && prof_enabled();
    if (!sWalkTimed) {
        return;
    }
    sWalkReadCost = prof_clock_read_cost();
    for (i = 0; i < WALK_PART_COUNT; i++) {
        sWalkTicks[i] = 0;
        sWalkSpans[i] = 0;
    }
    sWalkPart = WALK_COMMANDS;
    sWalkLast = plat_ticks();
}

static void walk_end(void) {
    unsigned long long perSecond, readTicks = 0;
    s32 i;
    if (!sWalkTimed) {
        return;
    }
    sWalkTicks[sWalkPart] += plat_ticks() - sWalkLast;
    sWalkSpans[sWalkPart]++;
    sWalkTimed = FALSE;
    perSecond = plat_ticks_per_second();
    for (i = 0; i < WALK_PART_COUNT; i++) {
        unsigned long long reads = sWalkSpans[i] * sWalkReadCost / 256;
        unsigned long long ticks = sWalkTicks[i] > reads ? sWalkTicks[i] - reads : 0;
        readTicks += sWalkTicks[i] - ticks;
        if (i < WALK_REPORTED) {
            prof_part(PROF_PART_WALK_COMMANDS + i, ticks * 1000000000ULL / perSecond);
        }
    }
    prof_part(PROF_PART_WALK_TIMER, readTicks * 1000000000ULL / perSecond);
}

/******************************** Helpers ********************************/

static inline const u8 *seg_addr(u32 addr) {
    // DKR only ever passes plain pointers (segment 0 has a zero base).
    return (const u8 *) (uintptr_t) addr;
}

static inline void unpack_rgba(u32 c, u8 *out) {
    out[0] = c >> 24;
    out[1] = c >> 16;
    out[2] = c >> 8;
    out[3] = c;
}

/******************************** Textures ********************************/

/*
 * Big enough to hold every texture a level has loaded (DKR allows 700) plus the variants drawn with other wrap modes,
 * so that entries are only replaced once the game has freed their textures.
 */
#define TEX_CACHE_SIZE 2048
#define TEX_CACHE_PROBES 8

typedef struct TexCacheEntry {
    const u8 *addr;
    const u8 *tlut;
    u32 hash;
    u16 width, height;
    u8 fmt, siz, lineSwapped, used;
    u8 wrapS, wrapT, tlutType;
    u8 gen; // Moves on when the entry is freed, so that the lookup memo notices (tex_memo_check)
    u32 lastFrame;
    GfxTexture tex;
    GfxTexture otherFilter; // The same texture with the other filter, sharing its GPU copy (when hasOtherFilter)
    u8 hasOtherFilter;
} TexCacheEntry;

static TexCacheEntry sTexCache[TEX_CACHE_SIZE];
/* Each entry's texture address (NULL when unused), packed so that dropping freed textures doesn't read every entry. */
static const u8 *sTexCacheAddr[TEX_CACHE_SIZE];
static s32 sPrewarming;       // Decoding textures the game has just loaded, outside of any display list
static u32 sRecentEvictions;  // Entries replaced although drawn in the last 60 frames (should stay 0)

/* A cheap hash of the texture data, used to notice when memory has been reused for something else. */
/* Out of line: a texture is hashed once in DKR_TEX_REHASH frames, so the lookups' code doesn't carry the loop. */
static NOINLINE u32 sample_hash(const u8 *data, u32 size) {
    u32 h = 2166136261u ^ size;
    u32 step = (size > 256) ? size / 64 : 1;
    u32 i;
    for (i = 0; i < size; i += step) {
        h = (h ^ data[i]) * 16777619u;
    }
    return h;
}

static inline u32 rgba(u32 r, u32 g, u32 b, u32 a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

static inline u32 rgba5551_to_8888(u16 c) {
    u32 r = (c >> 11) & 0x1F, gg = (c >> 6) & 0x1F, b = (c >> 1) & 0x1F;
    return rgba((r << 3) | (r >> 2), (gg << 3) | (gg >> 2), (b << 3) | (b >> 2), (c & 1) ? 255 : 0);
}

static inline u32 ia16_to_8888(u16 c) {
    u32 i = c >> 8, a = c & 0xFF;
    return rgba(i, i, i, a);
}

/* Reads byte "offset" of a texture row, undoing the odd line swap when needed. */
static inline u8 tex_byte(const TmemLoad *load, u32 row, u32 rowBytes, u32 x, u8 swapMask) {
    u32 offset;
    if (load->rowStride != 0) {
        offset = row * load->rowStride + x;
    } else {
        offset = row * rowBytes + x;
        if ((row & 1) && load->lineSwapped) {
            offset ^= swapMask;
        }
    }
    return load->addr[offset];
}

/* Bytes per texture row in RDRAM. For 32 bit textures TMEM splits each texel across two banks, so the tile's line
 * only counts half of the row. */
static inline u32 tile_row_bytes(const Tile *tile) {
    return tile->line * ((tile->siz == G_IM_SIZ_32b) ? 16 : 8);
}

/* Out of line (see decode_into_cache). */
__attribute__((noinline)) static void decode_texture(u32 *out, const TmemLoad *load, const Tile *tile, u32 width,
                                                     u32 height, const u8 *tlut) {
    u32 x, y;
    u32 rowBytes = tile_row_bytes(tile);
    u8 swapMask = (tile->siz == G_IM_SIZ_32b) ? 8 : 4;
    u8 b0, b1, b2, b3;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            u32 c = 0;
            switch (tile->siz) {
                case G_IM_SIZ_4b:
                    b0 = tex_byte(load, y, rowBytes, x >> 1, swapMask);
                    b0 = (x & 1) ? (b0 & 0xF) : (b0 >> 4);
                    if (tile->fmt == G_IM_FMT_CI) {
                        const u8 *p = tlut + b0 * 2; // tlut is the tile's palette (find_texture)
                        c = (g.omH & (3 << G_MDSFT_TEXTLUT)) == G_TT_IA16 ? ia16_to_8888((p[0] << 8) | p[1])
                                                                          : rgba5551_to_8888((p[0] << 8) | p[1]);
                    } else if (tile->fmt == G_IM_FMT_IA) {
                        // Widened by repeating the bits, as the RDP does.
                        u32 i3 = b0 >> 1, i = (i3 << 5) | (i3 << 2) | (i3 >> 1);
                        c = rgba(i, i, i, (b0 & 1) ? 255 : 0);
                    } else {
                        u32 i = b0 * 17;
                        c = rgba(i, i, i, i);
                    }
                    break;
                case G_IM_SIZ_8b:
                    b0 = tex_byte(load, y, rowBytes, x, swapMask);
                    if (tile->fmt == G_IM_FMT_CI) {
                        const u8 *p = tlut + b0 * 2;
                        c = (g.omH & (3 << G_MDSFT_TEXTLUT)) == G_TT_IA16 ? ia16_to_8888((p[0] << 8) | p[1])
                                                                          : rgba5551_to_8888((p[0] << 8) | p[1]);
                    } else if (tile->fmt == G_IM_FMT_IA) {
                        u32 i = (b0 >> 4) * 17, a = (b0 & 0xF) * 17;
                        c = rgba(i, i, i, a);
                    } else {
                        c = rgba(b0, b0, b0, b0);
                    }
                    break;
                case G_IM_SIZ_16b:
                    b0 = tex_byte(load, y, rowBytes, x * 2, swapMask);
                    b1 = tex_byte(load, y, rowBytes, x * 2 + 1, swapMask);
                    if (tile->fmt == G_IM_FMT_IA) {
                        c = ia16_to_8888((b0 << 8) | b1);
                    } else {
                        c = rgba5551_to_8888((b0 << 8) | b1);
                    }
                    break;
                case G_IM_SIZ_32b:
                    b0 = tex_byte(load, y, rowBytes, x * 4, swapMask);
                    b1 = tex_byte(load, y, rowBytes, x * 4 + 1, swapMask);
                    b2 = tex_byte(load, y, rowBytes, x * 4 + 2, swapMask);
                    b3 = tex_byte(load, y, rowBytes, x * 4 + 3, swapMask);
                    c = rgba(b0, b1, b2, b3);
                    break;
            }
            out[y * width + x] = c;
        }
    }
}

/* The texture format decode_texture's pixels fit exactly (see enum GfxTextureFormat). */
static u8 texture_format(const Tile *tile, u8 tlutType) {
    switch (tile->siz) {
        case G_IM_SIZ_4b:
            if (tile->fmt == G_IM_FMT_CI) {
                return (tlutType == (G_TT_IA16 >> G_MDSFT_TEXTLUT)) ? GFX_TEXFMT_LA8 : GFX_TEXFMT_RGBA5551;
            }
            // IA4 has 3 bit intensity, which 4 bits don't widen to exactly.
            return (tile->fmt == G_IM_FMT_IA) ? GFX_TEXFMT_LA8 : GFX_TEXFMT_LA4;
        case G_IM_SIZ_8b:
            if (tile->fmt == G_IM_FMT_CI) {
                return (tlutType == (G_TT_IA16 >> G_MDSFT_TEXTLUT)) ? GFX_TEXFMT_LA8 : GFX_TEXFMT_RGBA5551;
            }
            return (tile->fmt == G_IM_FMT_IA) ? GFX_TEXFMT_LA4 : GFX_TEXFMT_LA8;
        case G_IM_SIZ_16b:
            return (tile->fmt == G_IM_FMT_IA) ? GFX_TEXFMT_LA8 : GFX_TEXFMT_RGBA5551;
        default:
            return GFX_TEXFMT_RGBA8;
    }
}

static u8 wrap_mode(u8 cm, u8 mask) {
    if ((cm & G_TX_CLAMP) || mask == 0) {
        return GFX_WRAP_CLAMP;
    }
    return (cm & G_TX_MIRROR) ? GFX_WRAP_MIRROR : GFX_WRAP_REPEAT;
}

/*
 * Whether a cache entry has to keep its texture: the batch waiting to be drawn uses it, or the draw being set up
 * ("pinned") already looked it up for its other tile. Replacing it would draw those triangles with another texture.
 */
static s32 texture_in_use(const TexCacheEntry *e, const GfxTexture *pinned) {
    const GfxTexture *other = e->hasOtherFilter ? &e->otherFilter : &e->tex;
    if (&e->tex == pinned || other == pinned) {
        return TRUE;
    }
    if ((sModelCount > 0 || sMeshPartCount > 0) && (&e->tex == sModelState.textures[0] || &e->tex == sModelState.textures[1] ||
                            other == sModelState.textures[0] || other == sModelState.textures[1])) {
        return TRUE;
    }
    return sBatchCount > 0 && (&e->tex == sBatchState.textures[0] || &e->tex == sBatchState.textures[1] ||
                               other == sBatchState.textures[0] || other == sBatchState.textures[1]);
}

static void tex_memo_forget(void);

/* Frees a cache entry's textures. */
static void free_entry(TexCacheEntry *e) {
    if (e->hasOtherFilter) {
        gfx_split_texture_free(&e->otherFilter);
        e->hasOtherFilter = FALSE;
    }
    gfx_split_texture_free(&e->tex);
    plat_free((void *) e->tex.pixels);
    e->used = FALSE;
    e->gen++;
    if (e->gen == 0) {
        tex_memo_forget(); // The 8-bit generation wrapped: a memo entry from 256 frees ago must not match it again
    }
    sTexCacheAddr[e - sTexCache] = NULL;
}

/*
 * The entry's texture for drawing with the given filter. Mostly DKR draws a texture with one filter, but HUD and
 * menu textures are drawn both ways, and a texture decoded at load time doesn't know yet which it will be. The other
 * filter is a second texture object sharing the decoded pixels and GPU copy, so it costs no decode or upload; it
 * can't be the same object, since changing the filter in place would also change it for triangles already batched.
 */
__attribute__((noinline)) static GfxTexture *share_other_filter(TexCacheEntry *e, u8 filter) {
    e->otherFilter = e->tex;
    e->otherFilter.filter = filter;
    e->otherFilter.backendData = NULL;
    gfx_split_texture_share(&e->otherFilter, &e->tex);
    e->hasOtherFilter = TRUE;
    return &e->otherFilter;
}

static inline GfxTexture *texture_with_filter(TexCacheEntry *e, u8 filter) {
    if (e->tex.filter == filter) {
        return &e->tex;
    }
    if (!e->hasOtherFilter) {
        return share_other_filter(e, filter); // Once a texture: kept out of the lookups' code
    }
    return &e->otherFilter;
}

/*
 * sample_hash() for this frame. The same texture is often loaded many times in a frame, and RDRAM doesn't change
 * while a display list runs, so its hash is only worked out the first time. With DKR_TEX_REHASH=n (Old 3DS work,
 * roadmap O8) a texture's hash is kept for n frames: its samples are cache misses all over memory, which an Old 3DS,
 * without a second level cache, pays dearly for (texture lookups were ~3.5 ms of its frame, #223). Textures the game
 * frees are dropped from the cache anyway (prewarm_textures); a texture rewritten in place would show up to n - 1
 * frames late. Palettes are always hashed every frame.
 */
typedef struct HashCacheEntry {
    const u8 *data;
    u32 size, frame, hash;
} HashCacheEntry;

#define HASH_CACHE_SIZE 256

static HashCacheEntry sHashCache[HASH_CACHE_SIZE];

static s32 sTexRehash = 1; // DKR_TEX_REHASH: frames a texture's hash is kept

/* Out of line: one copy, not one for the texture and one for the palette in each caller. */
static NOINLINE HOT u32 frame_hash(const u8 *data, u32 size, u32 keepFrames) {
    HashCacheEntry *h = &sHashCache[(((uintptr_t) data >> 3) ^ size) % HASH_CACHE_SIZE];
    if ((u32) sFrameCount - h->frame >= keepFrames || h->data != data || h->size != size) {
        h->data = data;
        h->size = size;
        h->frame = (u32) sFrameCount;
        h->hash = sample_hash(data, size);
    }
    return h->hash;
}

static inline u32 cycle_type(void) {
    return g.omH & (3 << G_MDSFT_CYCLETYPE);
}

/* The filter the other mode draws textures with. Copy mode never filters. */
static inline u8 tex_filter(void) {
    return ((g.omH & (3 << G_MDSFT_TEXTFILT)) == G_TF_POINT || cycle_type() == G_CYC_COPY) ? GFX_FILTER_POINT
                                                                                            : GFX_FILTER_BILINEAR;
}

/*
 * The palette a tile's texture is decoded with: NULL unless it is colour indexed, and its bytes that can change the
 * texture (for the hash). Returns FALSE when a colour indexed tile has no palette loaded.
 */
static inline s32 tile_palette(const Tile *tile, const u8 **tlut, u32 *hashBytes) {
    *tlut = NULL;
    *hashBytes = 0;
    if (tile->fmt == G_IM_FMT_CI) {
        // The palette's first entry: CI4 tiles pick one of 16 palettes of 16, CI8 uses all 256 entries. The TLUT
        // load put entries from g.tlutFirst on, g.tlutCount of them.
        s32 first = (tile->siz == G_IM_SIZ_4b ? tile->palette * 16 : 0) - g.tlutFirst;
        s32 entries = tile->siz == G_IM_SIZ_4b ? 16 : 256;
        if (g.tlutAddr == NULL || first < 0 || first >= (s32) g.tlutCount) {
            return FALSE;
        }
        if (entries > (s32) g.tlutCount - first) {
            // CI8 with a short palette (DKR loads 64 entries): only those can change the texture.
            entries = (s32) g.tlutCount - first;
        }
        *tlut = g.tlutAddr + first * 2;
        *hashBytes = entries * 2;
    }
    return TRUE;
}

/*
 * A memo of texture lookups (DKR_TEX_MEMO, Old 3DS work, roadmap O8): a draw's texture is nearly always the one the
 * same tile and TMEM state resolved to a moment ago, so the answer is remembered under everything find_texture goes
 * by apart from the texture bytes and the cache itself. An answer confirmed earlier in this frame is returned as it
 * is (a texture's hash doesn't change within a frame, and a freed entry shows in its generation); one from an earlier
 * frame is confirmed the way find_texture would, by the entry's hash against frame_hash (cheap: the hash cache mostly
 * still holds it). Anything else goes through find_texture, which refreshes the memo. The lookups were 3.3-4.1 ms of
 * an Old 3DS frame (#223), mostly find_texture's 7 KB of code and the cache entries it probes missing the caches; a
 * memo hit runs a few dozen instructions over a set of two 32 byte memo entries and the cache entry's first line.
 * Two way: a Jungle Falls race frame looks up ~130 textures under ~77 keys, of which a direct mapped memo of 256
 * entries lost ~17 a frame to collisions, the two way one ~4 (PC, 900 frames).
 */
#define TEX_MEMO_SETS 256
#define TEX_MEMO_WAYS 2

typedef struct TexMemoEntry {
    // The key: the TMEM load, the palette as tile_palette derives it, the tile (of its coordinates only the size
    // matters to the lookup, so a texture that scrolls keeps its key) and the other mode's texture bits.
    const u8 *addr;
    const u8 *tlut;
    u32 load;  // size | rowStride << 16 (tex_memo_key skips loads that don't fit)
    u32 dims;  // lrs - uls | (lrt - ult) << 16
    u32 tileA; // line | tmem << 9 | the palette bytes hashed << 18
    u32 tileB; // fmt, siz, palette, cms, cmt, masks, maskt, lineSwapped and the other mode's bits (tex_memo_key)
    // The answer.
    u32 frame; // The frame it was last confirmed in
    u32 entry; // The cache entry's index | its gen << 11 | the texture's height as bounded by the load << 19
} __attribute__((aligned(32))) TexMemoEntry;

static TexMemoEntry sTexMemo[TEX_MEMO_SETS * TEX_MEMO_WAYS];
static s32 sTexMemoOn; // DKR_TEX_MEMO

/* Empties the memo (a zeroed entry has no load address, which no key has). */
static COLD void tex_memo_forget(void) {
    u32 *w = (u32 *) sTexMemo, n = sizeof(sTexMemo) / (sizeof(u32));
    while (n--) {
        *w++ = 0;
    }
}

/* The other mode bits the lookup goes by, as tex_memo_key packs them: filter, TLUT type and cycle type (copy mode). */
static inline u32 tex_memo_mode_bits(void) {
    return ((g.omH >> G_MDSFT_TEXTFILT) & 0xF) | ((g.omH >> (G_MDSFT_CYCLETYPE - 4)) & 0x30);
}

/* Builds the key for a tile, or returns FALSE when it isn't memoed: the tile's TMEM slot isn't a load's own (then
 * find_texture searches the others), the load is too big for the key, or it is colour indexed without a palette. */
static inline s32 tex_memo_key(TexMemoEntry *k, const Tile *tile) {
    const TmemLoad *load = &g.tmem[(tile->tmem / 8) & 63];
    u32 tlutHashBytes;
    if (!load->valid || (load->size | load->rowStride) > 0xFFFF || !tile_palette(tile, &k->tlut, &tlutHashBytes)) {
        return FALSE;
    }
    k->addr = load->addr;
    k->load = load->size | (load->rowStride << 16);
    k->dims = ((tile->lrs - tile->uls) & 0xFFFF) | ((u32) (tile->lrt - tile->ult) << 16);
    k->tileA = tile->line | (tile->tmem << 9) | (tlutHashBytes << 18);
    k->tileB = tile->fmt | (tile->siz << 3) | (tile->palette << 5) | (tile->cms << 9) | (tile->cmt << 11) |
               (tile->masks << 13) | (tile->maskt << 17) | ((u32) load->lineSwapped << 21) |
               (tex_memo_mode_bits() << 22);
    return TRUE;
}

/* The set a key lives in: TEX_MEMO_WAYS entries. */
static inline TexMemoEntry *tex_memo_set(const TexMemoEntry *k) {
    u32 h = (u32) (uintptr_t) k->addr ^ (u32) (uintptr_t) k->tlut ^ k->load ^ k->dims ^ k->tileA ^ k->tileB;
    return &sTexMemo[((h * 2654435761u) >> 24) * TEX_MEMO_WAYS];
}

static inline s32 tex_memo_same_key(const TexMemoEntry *m, const TexMemoEntry *k) {
    return m->addr == k->addr && m->tlut == k->tlut && m->load == k->load && m->dims == k->dims &&
           m->tileA == k->tileA && m->tileB == k->tileB;
}

/* Confirms a memo entry from an earlier frame: the cache entry still holds the texture, with the hash the bytes have
 * now. The frame_hash calls keep this out of the hit path's code. */
static NOINLINE HOT GfxTexture *tex_memo_confirm(TexMemoEntry *m, const TexMemoEntry *k, TexCacheEntry *e) {
    u32 rowStride = k->load >> 16, height = m->entry >> 19;
    u32 size = rowStride ? rowStride * height : (k->load & 0xFFFF);
    u32 hash = frame_hash(k->addr, size, (u32) sTexRehash);
    if (k->tlut != NULL) {
        hash ^= frame_hash(k->tlut, k->tileA >> 18, 1) * 31;
    }
    if (!e->used || e->addr != k->addr || e->hash != hash) {
        return NULL;
    }
    e->lastFrame = sFrameCount;
    m->frame = (u32) sFrameCount;
    return texture_with_filter(e, tex_filter());
}

/* The texture a memo entry (whose key matched) remembers, or NULL when it needs the full lookup again. */
static inline GfxTexture *tex_memo_check(TexMemoEntry *m, const TexMemoEntry *k) {
    TexCacheEntry *e = &sTexCache[m->entry & (TEX_CACHE_SIZE - 1)];
    if (e->gen != ((m->entry >> 11) & 0xFF) || e->addr != k->addr) {
        return NULL; // Freed since (and perhaps reused)
    }
    if (m->frame != (u32) sFrameCount) {
        return tex_memo_confirm(m, k, e);
    }
    return texture_with_filter(e, tex_filter());
}

/* The entry of a set that holds the key, or else the one for find_texture's answer: the one confirmed longest ago. */
static inline TexMemoEntry *tex_memo_find(TexMemoEntry *set, const TexMemoEntry *k, s32 *found) {
    TexMemoEntry *m, *oldest = set;
    for (m = set; m < set + TEX_MEMO_WAYS; m++) {
        if (tex_memo_same_key(m, k)) {
            *found = TRUE;
            return m;
        }
        if (m->frame < oldest->frame) {
            oldest = m;
        }
    }
    *found = FALSE;
    return oldest;
}

/* find_texture's answer, for the memo (when the lookup is memoed). */
static inline void tex_memo_note(TexMemoEntry *k, const TexCacheEntry *e, u32 height) {
    if (k != NULL) {
        k->frame = (u32) sFrameCount;
        k->entry = (u32) (e - sTexCache) | ((u32) e->gen << 11) | (height << 19);
    }
}

/*
 * Decodes a tile's texture into a cache entry (freeing what it held) and uploads it, or returns NULL when there is no
 * memory for it. Out of line, and with the backend's upload inlined into it rather than into find_texture: a lookup
 * that misses the memo but hits the cache then only fetches find_texture's own code (roadmap O8).
 */
__attribute__((noinline)) static TexCacheEntry *decode_into_cache(TexCacheEntry *e, const TmemLoad *load,
                                                                  const Tile *tile, u32 width, u32 height,
                                                                  const u8 *tlut, u32 hash) {
    u8 tlutType = (tlut != NULL) ? (g.omH >> G_MDSFT_TEXTLUT) & 3 : 0;
    u8 wrapS = wrap_mode(tile->cms, tile->masks);
    u8 wrapT = wrap_mode(tile->cmt, tile->maskt);
    u32 *pixels;
    unsigned long long texStart;

    walk_switch(WALK_DECODE); // Timed as PROF_TEXTURE instead
    texStart = plat_time_ns();
    if (e->used) {
        if (sFrameCount - (s32) e->lastFrame < 60) {
            sRecentEvictions++;
        }
        free_entry(e);
    }
    pixels = plat_malloc(width * height * 4);
    if (pixels == NULL) {
        gPlatCounters[PLAT_COUNTER_TEXTURE_FAILED]++;
        e->used = FALSE;
        prof_add(PROF_TEXTURE, plat_time_ns() - texStart);
        walk_switch(WALK_TEXTURES);
        return NULL;
    }
    decode_texture(pixels, load, tile, width, height, tlut);
    e->used = TRUE;
    e->addr = load->addr;
    sTexCacheAddr[e - sTexCache] = load->addr;
    e->tlut = tlut;
    e->hash = hash;
    e->width = width;
    e->height = height;
    e->fmt = tile->fmt;
    e->siz = tile->siz;
    e->lineSwapped = load->lineSwapped;
    e->wrapS = wrapS;
    e->wrapT = wrapT;
    e->tlutType = tlutType;
    e->lastFrame = sFrameCount;
    e->tex.width = width;
    e->tex.height = height;
    e->tex.pixels = (const unsigned int *) pixels;
    e->tex.format = texture_format(tile, tlutType);
    e->tex.filter = tex_filter();
    e->tex.wrapS = wrapS;
    e->tex.wrapT = wrapT;
    e->tex.backendData = NULL;
    gfx_split_texture_upload(&e->tex);
    prof_add(PROF_TEXTURE, plat_time_ns() - texStart);
    walk_switch(WALK_TEXTURES);
    if (!sPrewarming) {
        // Only the decodes that interrupt a frame: these are the hitches.
        prof_count(PROF_TEXTURE_UPLOADS, 1);
    }
    return e;
}

/* Returns the texture for the given tile, decoding it if it isn't in the cache. memo, when not NULL, takes what it
 * resolved to. Out of line, so that lookup_texture's memo path doesn't carry its code. */
__attribute__((noinline)) static GfxTexture *find_texture(s32 tileIndex, const GfxTexture *pinned, TexMemoEntry *memo) {
    Tile *tile = &g.tiles[tileIndex & 7];
    TmemLoad *load = &g.tmem[(tile->tmem / 8) & 63], shifted;
    u32 width, height, size, hash, i, slot, oldest;
    const u8 *tlut;
    u32 tlutHashBytes;
    u8 filter, wrapS, wrapT, tlutType;
    TexCacheEntry *e;

    if (!load->valid) {
        // Loaded at an unaligned TMEM address, or inside a bigger load. Search for the load covering it, and start
        // where the tile does.
        for (i = 0; i < 64; i++) {
            if (g.tmem[i].valid && i * 8 <= tile->tmem && tile->tmem < i * 8 + (g.tmem[i].size + 7) / 8) {
                u32 skip = (tile->tmem - i * 8) * 8;
                shifted = g.tmem[i];
                if (shifted.rowStride == 0) {
                    shifted.addr += skip;
                    shifted.size -= skip;
                } else if (skip % shifted.rowStride == 0) {
                    shifted.addr += skip;
                    shifted.size -= skip;
                } else {
                    break;
                }
                load = &shifted;
                break;
            }
        }
        if (!load->valid) {
            return NULL;
        }
    }
    width = ((tile->lrs - tile->uls) >> 2) + 1;
    height = ((tile->lrt - tile->ult) >> 2) + 1;
    if (width > 1024 || height > 1024 || width == 0 || height == 0) {
        return NULL;
    }
    if (tile->line == 0 && load->rowStride == 0) {
        return NULL;
    }
    // Bound the texture by what was actually loaded.
    if (load->rowStride != 0 && height > load->size / load->rowStride) {
        height = load->size / load->rowStride;
        if (height == 0) {
            return NULL;
        }
    } else if (load->rowStride == 0 && tile->line != 0) {
        u32 rows = load->size / tile_row_bytes(tile);
        if (rows == 0) {
            rows = 1;
        }
        if (height > rows) {
            height = rows;
        }
    }
    if (!tile_palette(tile, &tlut, &tlutHashBytes)) {
        return NULL;
    }
    size = load->rowStride ? load->rowStride * height : load->size;
    hash = frame_hash(load->addr, size, (u32) sTexRehash);
    if (tlut != NULL) {
        hash ^= frame_hash(tlut, tlutHashBytes, 1) * 31;
    }
    filter = tex_filter();
    tlutType = (tlut != NULL) ? (g.omH >> G_MDSFT_TEXTLUT) & 3 : 0;
    wrapS = wrap_mode(tile->cms, tile->masks);
    wrapT = wrap_mode(tile->cmt, tile->maskt);

    slot = ((uintptr_t) load->addr >> 3) % TEX_CACHE_SIZE;
    oldest = TEX_CACHE_SIZE;
    for (i = 0; i < TEX_CACHE_PROBES; i++) {
        e = &sTexCache[(slot + i) % TEX_CACHE_SIZE];
        // The wrap modes are part of the key, since changing them in place would also change them for triangles
        // already batched with this texture. The filter isn't: see texture_with_filter.
        if (e->used && e->addr == load->addr && e->width == width && e->height == height && e->fmt == tile->fmt &&
            e->siz == tile->siz && e->tlut == tlut && e->hash == hash && e->lineSwapped == load->lineSwapped &&
            e->wrapS == wrapS && e->wrapT == wrapT && e->tlutType == tlutType) {
            e->lastFrame = sFrameCount;
            tex_memo_note(memo, e, height);
            return texture_with_filter(e, filter);
        }
        if (!e->used) {
            oldest = (slot + i) % TEX_CACHE_SIZE;
            break;
        }
        if (!texture_in_use(e, pinned) && (oldest == TEX_CACHE_SIZE || e->lastFrame < sTexCache[oldest].lastFrame)) {
            oldest = (slot + i) % TEX_CACHE_SIZE;
        }
    }
    if (oldest == TEX_CACHE_SIZE) {
        return NULL; // Can't happen: at most three of the eight entries are in use.
    }

    e = decode_into_cache(&sTexCache[oldest], load, tile, width, height, tlut, hash);
    if (e == NULL) {
        return NULL;
    }
    tex_memo_note(memo, e, height);
    return &e->tex;
}

/* The texture the tile shows: from the memo, the cache, or decoded into it. Out of line: one copy of the memo's code
 * instead of one per call in make_draw_state. The memo sits out prewarm_textures, whose lookups are one-offs against
 * a scratch tile and TMEM state. */
static NOINLINE HOT GfxTexture *lookup_texture(s32 tileIndex, const GfxTexture *pinned) {
    u32 part = walk_switch(WALK_TEXTURES);
    TexMemoEntry key, *m = NULL;
    GfxTexture *tex = NULL;
    if (sTexMemoOn && !sPrewarming && tex_memo_key(&key, &g.tiles[tileIndex & 7])) {
        s32 found;
        m = tex_memo_find(tex_memo_set(&key), &key, &found);
        if (found) {
            tex = tex_memo_check(m, &key);
        }
    }
    if (tex == NULL) {
        tex = find_texture(tileIndex, pinned, m != NULL ? &key : NULL);
        if (m != NULL && tex != NULL) {
            *m = key;
        }
    }
    walk_switch(part);
    return tex;
}

/*
 * Converts texture coordinates in 10.5 texels to normalised coordinates of the tile's texture. The per-draw part is
 * worked out once, so there are no divisions per vertex (slow on the 3DS's VFP), and the results are the same as
 * dividing each time: the shift and the 1/32 are powers of two, so one combined factor is exact, and so is the
 * reciprocal of a power of two texture size (other sizes still divide). For a power of two size the reciprocal is
 * folded into the factor and the offset too, so a coordinate costs one multiply and one subtract: both products are
 * exact (powers of two scale a float's bits without rounding, and no value gets near the subnormals), and the one
 * rounding, the subtraction's, lands on the same bits whether the operands were scaled before it or the result after.
 */
typedef struct TexAxis {
    float scale, offset, size;
    s32 divide;
} TexAxis;

/* The RDP's bilinear filter puts texel centres on whole texture coordinates, while GPUs (and the software renderer)
 * put them half a texel further on, so filtered textures are moved by that half texel to land where the N64 draws them.
 * Point sampled ones line up already. */
static inline float texel_centre_shift(const GfxTexture *tex) {
    return (tex->filter == GFX_FILTER_BILINEAR) ? 0.5f : 0.0f;
}

static NOINLINE HOT void tex_axis_init(TexAxis *a, u8 shift, u16 tileStart, u32 size, float centreShift) {
    float scale = 1.0f / 32.0f;
    float offset = tileStart * 0.25f - centreShift;
    if (shift > 10) {
        scale *= (float) (1 << (16 - shift));
    } else if (shift > 0) {
        scale /= (float) (1 << shift);
    }
    a->size = (float) size;
    a->divide = (size & (size - 1)) != 0;
    if (!a->divide) {
        float invSize = 1.0f / (float) size;
        scale *= invSize;
        offset *= invSize;
    }
    a->scale = scale;
    a->offset = offset;
}

static inline float tex_axis_coord(const TexAxis *a, s32 coord) {
    float c = coord * a->scale - a->offset;
    if (UNLIKELY(a->divide)) {
        c /= a->size;
    }
    return c;
}

/******************************** Combiner and blender ********************************/

static const u8 sCCRgbA[16] = { GFX_CC_COMBINED, GFX_CC_TEXEL0, GFX_CC_TEXEL1, GFX_CC_PRIM, GFX_CC_SHADE, GFX_CC_ENV,
                                GFX_CC_ONE,      GFX_CC_NOISE,  GFX_CC_ZERO,   GFX_CC_ZERO, GFX_CC_ZERO,  GFX_CC_ZERO,
                                GFX_CC_ZERO,     GFX_CC_ZERO,   GFX_CC_ZERO,   GFX_CC_ZERO };
static const u8 sCCRgbB[16] = { GFX_CC_COMBINED, GFX_CC_TEXEL0, GFX_CC_TEXEL1, GFX_CC_PRIM, GFX_CC_SHADE, GFX_CC_ENV,
                                GFX_CC_KEY_CENTER, GFX_CC_K4,   GFX_CC_ZERO,   GFX_CC_ZERO, GFX_CC_ZERO,  GFX_CC_ZERO,
                                GFX_CC_ZERO,     GFX_CC_ZERO,   GFX_CC_ZERO,   GFX_CC_ZERO };
static const u8 sCCRgbC[32] = { GFX_CC_COMBINED,       GFX_CC_TEXEL0,       GFX_CC_TEXEL1,       GFX_CC_PRIM,
                                GFX_CC_SHADE,          GFX_CC_ENV,          GFX_CC_KEY_SCALE,    GFX_CC_COMBINED_ALPHA,
                                GFX_CC_TEXEL0_ALPHA,   GFX_CC_TEXEL1_ALPHA, GFX_CC_PRIM_ALPHA,   GFX_CC_SHADE_ALPHA,
                                GFX_CC_ENV_ALPHA,      GFX_CC_LOD_FRACTION, GFX_CC_PRIM_LOD_FRACTION, GFX_CC_K5,
                                GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO,
                                GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO, GFX_CC_ZERO,
                                GFX_CC_ZERO, GFX_CC_ZERO };
static const u8 sCCRgbD[8] = { GFX_CC_COMBINED, GFX_CC_TEXEL0, GFX_CC_TEXEL1, GFX_CC_PRIM,
                               GFX_CC_SHADE,    GFX_CC_ENV,    GFX_CC_ONE,    GFX_CC_ZERO };
static const u8 sCCAlphaABD[8] = { GFX_CC_COMBINED, GFX_CC_TEXEL0, GFX_CC_TEXEL1, GFX_CC_PRIM,
                                   GFX_CC_SHADE,    GFX_CC_ENV,    GFX_CC_ONE,    GFX_CC_ZERO };
static const u8 sCCAlphaC[8] = { GFX_CC_LOD_FRACTION, GFX_CC_TEXEL0, GFX_CC_TEXEL1,            GFX_CC_PRIM,
                                 GFX_CC_SHADE,        GFX_CC_ENV,    GFX_CC_PRIM_LOD_FRACTION, GFX_CC_ZERO };

/* Which texel a combiner input reads: bit 0 texel 0, bit 1 texel 1. */
static const u8 sCCTexelUse[GFX_CC_INPUT_COUNT] = {
    [GFX_CC_TEXEL0] = 1, [GFX_CC_TEXEL0_ALPHA] = 1, [GFX_CC_TEXEL1] = 2, [GFX_CC_TEXEL1_ALPHA] = 2,
};

static inline u8 equation_texel_use(const GfxCCEquation *eq) {
    return sCCTexelUse[eq->a] | sCCTexelUse[eq->b] | sCCTexelUse[eq->c] | sCCTexelUse[eq->d];
}

/* Not HOT: it runs for a G_SETCOMBINE, a third as often as a draw. */
static NOINLINE void decode_combiner(void) {
    u32 w0 = g.combineW0, w1 = g.combineW1;
    GfxCombiner *cc = &g.combiner;
    u8 uses;

    cc->rgb[0].a = sCCRgbA[(w0 >> 20) & 0xF];
    cc->rgb[0].b = sCCRgbB[(w1 >> 28) & 0xF];
    cc->rgb[0].c = sCCRgbC[(w0 >> 15) & 0x1F];
    cc->rgb[0].d = sCCRgbD[(w1 >> 15) & 0x7];
    cc->alpha[0].a = sCCAlphaABD[(w0 >> 12) & 0x7];
    cc->alpha[0].b = sCCAlphaABD[(w1 >> 12) & 0x7];
    cc->alpha[0].c = sCCAlphaC[(w0 >> 9) & 0x7];
    cc->alpha[0].d = sCCAlphaABD[(w1 >> 9) & 0x7];
    cc->rgb[1].a = sCCRgbA[(w0 >> 5) & 0xF];
    cc->rgb[1].b = sCCRgbB[(w1 >> 24) & 0xF];
    cc->rgb[1].c = sCCRgbC[(w0 >> 0) & 0x1F];
    cc->rgb[1].d = sCCRgbD[(w1 >> 6) & 0x7];
    cc->alpha[1].a = sCCAlphaABD[(w1 >> 21) & 0x7];
    cc->alpha[1].b = sCCAlphaABD[(w1 >> 3) & 0x7];
    cc->alpha[1].c = sCCAlphaC[(w1 >> 18) & 0x7];
    cc->alpha[1].d = sCCAlphaABD[(w1 >> 0) & 0x7];

    // Which textures the combiner reads, so draws don't have to work it out each time.
    g.combinerTextures[0] = 0;
    uses = equation_texel_use(&cc->rgb[0]) | equation_texel_use(&cc->alpha[0]);
    g.combinerTextures[1] = uses;
    uses |= equation_texel_use(&cc->rgb[1]) | equation_texel_use(&cc->alpha[1]);
    g.combinerTextures[2] = uses;
}

/* The scissor's right edge for rectangles: exclusive, except in fill and copy modes, where the RDP also draws the
 * column at the edge. */
static inline s32 scissor_right(u32 cyc) {
    s32 right = g.scissor[2];
    if ((cyc == G_CYC_FILL || cyc == G_CYC_COPY) && right < GFX_SCREEN_WIDTH) {
        right++;
    }
    return right;
}

/* Builds the draw state for the current RSP/RDP state. Out of line: one copy, for triangles and rectangles. */
static NOINLINE HOT void make_draw_state(GfxDrawState *s, s32 isRect) {
    u32 cyc = cycle_type();
    u32 omL = g.omL;
    u32 blendBits;
    u32 p, a, m, b;
    s32 usesTex0 = FALSE, usesTex1 = FALSE;

    s->combiner = g.combiner;
    s->combiner.numCycles = (cyc == G_CYC_2CYCLE) ? 2 : 1;
    if (cyc == G_CYC_COPY) {
        // Copy mode writes texels straight to the framebuffer.
        s->combiner.rgb[0].a = GFX_CC_ZERO;
        s->combiner.rgb[0].b = GFX_CC_ZERO;
        s->combiner.rgb[0].c = GFX_CC_ZERO;
        s->combiner.rgb[0].d = GFX_CC_TEXEL0;
        s->combiner.alpha[0] = s->combiner.rgb[0];
        s->combiner.numCycles = 1;
    }
    __builtin_memcpy(s->prim, g.prim, 4);
    __builtin_memcpy(s->env, g.env, 4);
    __builtin_memcpy(s->fog, g.fogColor, 4);
    s->fogMul = g.fogMul;
    s->fogOffset = g.fogOffset;
    s->primLodFrac = g.primLodFrac;

    // Which textures does the combiner read? Copy mode reads texel 0, as set up above.
    if (cyc == G_CYC_COPY) {
        usesTex0 = TRUE;
    } else {
        usesTex0 = (g.combinerTextures[s->combiner.numCycles] & 1) != 0;
        usesTex1 = (g.combinerTextures[s->combiner.numCycles] & 2) != 0;
    }
    s->textures[0] = NULL;
    s->textures[1] = NULL;

    // Blender.
    blendBits = (cyc == G_CYC_2CYCLE) ? (omL >> 16) & 0x3333 : (omL >> 18) & 0x3333;
    p = (blendBits >> 12) & 3;
    a = (blendBits >> 8) & 3;
    m = (blendBits >> 4) & 3;
    b = blendBits & 3;
    s->fogEnabled = FALSE;
    if (cyc == G_CYC_2CYCLE) {
        u32 p0 = (omL >> 30) & 3, a0 = (omL >> 26) & 3;
        if (p0 == G_BL_CLR_FOG && a0 == G_BL_A_SHADE) {
            s->fogEnabled = TRUE;
        }
    } else if (cyc == G_CYC_1CYCLE) {
        if (p == G_BL_CLR_FOG && a == G_BL_A_SHADE) {
            // Fog only, no blending.
            s->fogEnabled = TRUE;
            p = G_BL_CLR_IN;
            m = G_BL_CLR_IN;
        }
    }
    s->blend = GFX_BLEND_OPAQUE;
    if (cyc == G_CYC_1CYCLE || cyc == G_CYC_2CYCLE) {
        if ((omL & FORCE_BL) && m == G_BL_CLR_MEM && p == G_BL_CLR_IN) {
            s->blend = (b == G_BL_1) ? GFX_BLEND_ADD : GFX_BLEND_ALPHA;
        }
    }
    s->alphaTest = FALSE;
    s->alphaRef = 0;
    if ((omL & 3) == G_AC_THRESHOLD) {
        s->alphaTest = TRUE;
        s->alphaRef = g.blendColor[3] ? g.blendColor[3] : 1;
    } else if ((omL & CVG_X_ALPHA) && s->blend == GFX_BLEND_OPAQUE) {
        s->alphaTest = TRUE;
        s->alphaRef = 128;
    }
    if (cyc == G_CYC_COPY) {
        s->blend = GFX_BLEND_OPAQUE;
        s->alphaTest = ((omL & 3) == G_AC_THRESHOLD);
        s->alphaRef = 1;
    }

    if (isRect) {
        s->depthTest = FALSE;
        s->depthWrite = FALSE;
        s->decal = FALSE;
        s->cullMode = 0;
        s->fogEnabled = FALSE;
        if (cyc != G_CYC_COPY && cyc != G_CYC_FILL && (omL & Z_CMP) && (omL & G_ZS_PRIM)) {
            s->depthTest = TRUE;
        }
    } else {
        s->depthTest = (g.geometryMode & G_ZBUFFER) && (omL & Z_CMP);
        s->depthWrite = (g.geometryMode & G_ZBUFFER) && (omL & Z_UPD);
        s->decal = (omL & ZMODE_DEC) == ZMODE_DEC;
        s->cullMode = 0;
    }
    __builtin_memcpy(s->scissor, g.scissor, sizeof(s->scissor));
    s->flat = isRect; // Triangles set it per triangle (draw_dkr_triangles).
    if (isRect) {
        s->scissor[2] = scissor_right(cyc);
    }

    if (usesTex0 || cyc == G_CYC_COPY) {
        s->textures[0] = lookup_texture(isRect ? (g.rdpHalf1 >> 0) & 7 : 0, NULL);
    }
    if (usesTex1) {
        s->textures[1] = lookup_texture(isRect ? ((g.rdpHalf1 + 1) & 7) : 1, s->textures[0]);
    }
}

/******************************** Batching ********************************/

static void build_draw_state(GfxDrawState *s, s32 isRect) {
    u32 part = walk_switch(WALK_DRAWSTATE);
    make_draw_state(s, isRect);
    walk_switch(part);
}

/* Compared a word at a time in place: a library memcmp is a call into code that is otherwise never in the
 * instruction cache, which is what a triangle command pays for here. The state's size is whole words. */
static inline s32 draw_states_equal(const GfxDrawState *a, const GfxDrawState *b) {
    const u32 *wa = (const u32 *) a, *wb = (const u32 *) b;
    u32 i;
    _Static_assert(sizeof(GfxDrawState) % 4 == 0 && _Alignof(GfxDrawState) >= 4, "draw states are compared as words");
#pragma GCC unroll 1
    for (i = 0; i < sizeof(GfxDrawState) / 4; i++) {
        if (wa[i] != wb[i]) {
            return FALSE;
        }
    }
    return TRUE;
}

static void trace_state(const GfxDrawState *st, s32 count) {
    TRACE("  DRAW %d tris: cc rgb0(%d,%d,%d,%d) a0(%d,%d,%d,%d) rgb1(%d,%d,%d,%d) a1(%d,%d,%d,%d) cyc%d tex %p %p "
          "prim %02X%02X%02X%02X env %02X%02X%02X%02X blend %d at %d/%d z %d/%d fog %d cull %d",
          (int) count, st->combiner.rgb[0].a, st->combiner.rgb[0].b, st->combiner.rgb[0].c, st->combiner.rgb[0].d,
          st->combiner.alpha[0].a, st->combiner.alpha[0].b, st->combiner.alpha[0].c, st->combiner.alpha[0].d,
          st->combiner.rgb[1].a, st->combiner.rgb[1].b, st->combiner.rgb[1].c, st->combiner.rgb[1].d,
          st->combiner.alpha[1].a, st->combiner.alpha[1].b, st->combiner.alpha[1].c, st->combiner.alpha[1].d,
          st->combiner.numCycles, (void *) st->textures[0], (void *) st->textures[1], st->prim[0], st->prim[1],
          st->prim[2], st->prim[3], st->env[0], st->env[1], st->env[2], st->env[3], st->blend, st->alphaTest,
          st->alphaRef, st->depthTest, st->depthWrite, st->fogEnabled, st->cullMode);
}

/* The traced frame's log of a batch about to be drawn. */
static COLD void trace_batch(void) {
    const GfxVertex *v = &sBatchVerts[0];
    float minU = v->u0, maxU = v->u0, minV = v->v0, maxV = v->v0, minW = v->w, maxW = v->w;
    s32 i;
    for (i = 1; i < sBatchVertices; i++) {
        const GfxVertex *o = &sBatchVerts[i];
        minU = (o->u0 < minU) ? o->u0 : minU;
        maxU = (o->u0 > maxU) ? o->u0 : maxU;
        minV = (o->v0 < minV) ? o->v0 : minV;
        maxV = (o->v0 > maxV) ? o->v0 : maxV;
        minW = (o->w < minW) ? o->w : minW;
        maxW = (o->w > maxW) ? o->w : maxW;
    }
    trace_state(&sBatchState, sBatchCount);
    plat_log("    %d vertices, v0 %.3f %.3f %.3f %.3f uv %.3f %.3f rgba %d %d %d %d", (int) sBatchVertices, v->x, v->y,
             v->z, v->w, v->u0, v->v0, v->r, v->g, v->b, v->a);
    plat_log("    u %.3f..%.3f v %.3f..%.3f w %.1f..%.1f", minU, maxU, minV, maxV, minW, maxW);
}

static COLD void flush_model_batch(void);
static COLD void flush_mesh_batch(void);

/* The draw mix (profiler "draw mix" line): what the draws are for, from the game's tags (port_drawtag.h), and per kind
 * the draws, triangles and vertices since the profiler last took them. Plain counters: the walk's thread writes them. */
static u8 sDrawKind;
static unsigned long sDrawMix[4][3];

void gfx_draw_mix_take(unsigned long out[4][3]) {
    __builtin_memcpy(out, sDrawMix, sizeof(sDrawMix));
    __builtin_memset(sDrawMix, 0, sizeof(sDrawMix));
}

/* Draws the batch of transformed triangles. */
static NOINLINE HOT void flush_cpu_batch(void) {
    if (sBatchCount > 0) {
        u32 part;
        if (UNLIKELY(sTracing)) {
            trace_batch();
        }
        part = walk_switch(WALK_DRAWS);
        gfx_split_draw_indexed(&sBatchState, sBatchVerts, sBatchVertices, sBatchIndices, sBatchCount);
        prof_count(PROF_TRIANGLES, sBatchCount);
        prof_count(PROF_DRAWS, 1);
        sDrawMix[sDrawKind][0]++;
        sDrawMix[sDrawKind][1] += sBatchCount;
        REF_CALL(refb_draw_indexed(&sBatchState, sBatchVerts, sBatchVertices, sBatchIndices, sBatchCount));
        walk_switch(part);
        sBatchCount = 0;
        sBatchVertices = 0;
        sBatchGeneration++;
    }
}

/* Draws whichever batch holds triangles. */
static inline void flush_batch(void) {
    if (sMeshPartCount > 0) {
        flush_mesh_batch();
    }
    if (sModelCount > 0) {
        flush_model_batch();
    }
    flush_cpu_batch();
}

/* Whether the batch has room for one more triangle with up to three new vertices. */
static inline s32 batch_has_room(void) {
    return sBatchCount < MAX_BATCH_TRIANGLES && sBatchVertices + 3 <= MAX_BATCH_VERTICES;
}

/* Makes the batch one of this state with room for the triangles and vertices, flushing it first if it is full or of
 * another state. */
static void batch_reserve(const GfxDrawState *state, s32 triangles, s32 vertices) {
    if (sModelCount > 0 || sMeshPartCount > 0) {
        flush_mesh_batch();
        flush_model_batch();
    }
    if (sBatchCount > 0 && (sBatchCount + triangles > MAX_BATCH_TRIANGLES ||
                            sBatchVertices + vertices > MAX_BATCH_VERTICES || !draw_states_equal(state, &sBatchState))) {
        flush_batch();
    }
    if (sBatchCount == 0) {
        sBatchState = *state;
    }
}

/* Starts a triangle in the batch of this state (see batch_reserve). Returns where its three indices go; the vertices
 * come from batch_vertex. */
static u16 *batch_triangle(const GfxDrawState *state) {
    batch_reserve(state, 1, 3);
    return &sBatchIndices[sBatchCount++ * 3];
}

/* Adds a vertex to the batch (which must have room) and returns its index. */
static inline u16 batch_vertex(GfxVertex **out) {
    *out = &sBatchVerts[sBatchVertices];
    return (u16) sBatchVertices++;
}

/******************************** RSP ********************************/

/* Kept a loop (not unrolled into 8 copies) so that it takes a few cache lines: it runs between draws. */
static NOINLINE HOT void load_matrix(const Mtx *mtx, s32 index) {
    const s32 *m = (const s32 *) mtx;
    float *out = &g.matrices[index][0][0];
    s32 k;
#pragma GCC unroll 1
    for (k = 0; k < 8; k++) {
        s32 intPart = m[k];
        s32 fracPart = m[8 + k];
        s32 a = (intPart & 0xFFFF0000) | ((fracPart >> 16) & 0xFFFF);
        s32 b = ((intPart << 16) & 0xFFFF0000) | (fracPart & 0xFFFF);
        out[2 * k] = a * (1.0f / 65536.0f);
        out[2 * k + 1] = b * (1.0f / 65536.0f);
    }
}

/* Fog, as the RSP computes it from the normalised depth. fogMul and fogOffset are g's (exact as floats). */
static inline u8 fog_factor(const RspVertex *v, float fogMul, float fogOffset) {
    float w = (v->w != 0.0f) ? v->w : 0.0001f;
    // Clamp before converting: out of range float to int conversions differ between CPUs.
    float fog = (v->z / w) * fogMul + fogOffset;
    return (fog < 0.0f) ? 0 : (fog > 255.0f) ? 255 : (u8) fog;
}

static void vertex_fog(RspVertex *v) {
    v->fog = fog_factor(v, g.fogMul, g.fogOffset);
}

#if defined(__arm__) && defined(__VFP_FP__) && !defined(__SOFTFP__)
/*
 * The vertex transform with the VFP's short vectors (roadmap P27, DKR_VFP_VECTORS=1): the ARM11's VFP runs one
 * instruction over 4 registers when the FPSCR's vector length is 4, so each vertex takes 4 arithmetic instructions
 * instead of 16. A vector instruction treats a source register in s0-s7 as a scalar, so the matrix's rows sit in
 * s8-s23 as vectors, the vertex's x, y and z in s0-s2 and the result in s24-s27. The sums are made in the same order
 * as the C loop's, and the VFP's multiply-accumulate rounds the product before adding, as a separate multiply and add
 * do, so the results are bit-identical. The compiler doesn't know about the vector length, so it is set and put back
 * within the one asm statement, which uses no other floating point instructions while it is set (an instruction
 * writing s0-s7 stays scalar). Only x, y, z and w are written; load_vertices does the rest.
 */
#define HAVE_VFP_VECTORS 1
static void transform_vertices_vfp(const Vertex *src, s32 count, RspVertex *dst, float (*m)[4]) {
    u32 savedFpscr, x, y, z;
    __asm__ volatile("vldmia %[m], {s8-s23}\n\t"
                     "vmrs %[saved], fpscr\n\t"
                     "bic %[x], %[saved], #0x00370000\n\t" // LEN (bits 16-18) and STRIDE (20-21)
                     "orr %[x], %[x], #0x00030000\n\t"     // LEN = 4, STRIDE = 1
                     "vmsr fpscr, %[x]\n"
                     "1:\n\t"
                     "ldrsh %[x], [%[src]]\n\t"
                     "ldrsh %[y], [%[src], #2]\n\t"
                     "ldrsh %[z], [%[src], #4]\n\t"
                     "add %[src], %[src], %[srcSize]\n\t"
                     "vmov s0, %[x]\n\t"
                     "vmov s1, %[y]\n\t"
                     "vmov s2, %[z]\n\t"
                     "vcvt.f32.s32 s0, s0\n\t"
                     "vcvt.f32.s32 s1, s1\n\t"
                     "vcvt.f32.s32 s2, s2\n\t"
                     "vmul.f32 s24, s8, s0\n\t"  // s24-s27 = row 0 * x
                     "vmla.f32 s24, s12, s1\n\t" // += row 1 * y
                     "vmla.f32 s24, s16, s2\n\t" // += row 2 * z
                     "vadd.f32 s24, s24, s20\n\t" // += row 3
                     "vstmia %[dst], {s24-s27}\n\t"
                     "add %[dst], %[dst], %[dstSize]\n\t"
                     "subs %[count], %[count], #1\n\t"
                     "bne 1b\n\t"
                     "vmsr fpscr, %[saved]"
                     : [src] "+r"(src), [dst] "+r"(dst), [count] "+r"(count), [saved] "=&r"(savedFpscr), [x] "=&r"(x),
                       [y] "=&r"(y), [z] "=&r"(z)
                     : [m] "r"(m), [srcSize] "I"(sizeof(Vertex)), [dstSize] "I"(sizeof(RspVertex))
                     : "d0", "d1", "d4", "d5", "d6", "d7", "d8", "d9", "d10", "d11", "d12", "d13", "cc", "memory");
}
#endif

static s32 sVfpVectors; // DKR_VFP_VECTORS: transform vertices with the VFP's short vectors (P27)
static s32 sWalkLean;   // DKR_WALK_LEAN: the walk's shortcuts (see read_switches)
static s32 sTrackMeshSwitch, sModelMeshSwitch; // DKR_TRACK_MESH (P31) and DKR_MODEL_MESH (P34), see mesh_triangles
static s32 sCpuCullSwitch; // DKR_CPU_CULL (see sCpuCull)
/* Back faces culled here rather than by the GPU (DKR_CPU_CULL, on by default on an Old 3DS only), this frame: a
 * command's double sided triangles no longer split its draw from the one sided ones (about one draw in eight), and
 * back faces cost no vertices, indices or GPU time. Off for 3D (stereo) frames, whose eye offset the test leaves out,
 * and for the host's clipping sweep, which looks at back faces. */
static s32 sCpuCull;
static s32 sSwitchEpoch = -1;

/* Reads the renderer's experimental switches when they change (see plat_switch). */
static void read_switches(void) {
    if (sSwitchEpoch == gPlatSwitchEpoch) {
        return;
    }
    sSwitchEpoch = gPlatSwitchEpoch;
    // On by default on an Old 3DS only: switch bench #238 there, 8% less renderer time per triangle (the New 3DS's
    // #176 showed nothing). Bit-identical to the C loop.
    sVfpVectors = plat_switch("DKR_VFP_VECTORS", plat_old_3ds()) != 0;
    // 8 frames on an Old 3DS (switch bench #241, with the system-call flush the best of its switches together).
    sTexRehash = plat_switch("DKR_TEX_REHASH", plat_old_3ds() ? 8 : 1);
    if (sTexRehash < 1) {
        sTexRehash = 1;
    }
    // Remembers what each tile's texture lookup resolved to (tex_memo_check). Stale entries are checked, so it can
    // be switched either way between frames.
    sTexMemoOn = plat_switch("DKR_TEX_MEMO", plat_old_3ds()) != 0;
    // Old 3DS work (roadmap O9): the walk skips what it can show it doesn't need (draw_dkr_triangles): the per
    // triangle flatness checks while no loaded vertex is flat, and zeroing the second texture's coordinates in draws
    // that have none. The frames are the same either way.
    sWalkLean = plat_switch("DKR_WALK_LEAN", plat_old_3ds()) != 0;
    // The profiler's breakdown of the walk (see sWalkTimed): off on an Old 3DS, where its clock reads are system calls
    // that cost the walk a few milliseconds a frame.
    sWalkParts = plat_switch("DKR_WALK_PARTS", !plat_old_3ds()) != 0;
    // The GPU vertex maths' meshes (P31 track, P34 models): Settings > Picture > GPU vertex maths (off by default).
    sTrackMeshSwitch = plat_switch("DKR_TRACK_MESH", plat_gpu_maths_setting() >= 2) != 0;
    sModelMeshSwitch = plat_switch("DKR_MODEL_MESH", plat_gpu_maths_setting() >= 3) != 0;
    sCpuCullSwitch = plat_switch("DKR_CPU_CULL", plat_old_3ds()) != 0;
}

/* The host's clipping sweep (DKR_CLIP_SWEEP) tells the level's own geometry from objects': nonzero while the
 * triangles being drawn use the level model's vertices. */
int gGfxSweep, gGfxSweepLevel;
extern s32 port_sweep_level_vertex(const void *vertices);

/* The vertex transform in C: the VFP's short vectors (transform_vertices_vfp) do the same on the 3DS. */
static NOINLINE void transform_vertices(const Vertex *src, s32 count, RspVertex *dst, float (*m)[4]) {
    s32 i;
    for (i = 0; i < count; i++) {
        float x = src[i].x, y = src[i].y, z = src[i].z;
        dst[i].x = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
        dst[i].y = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
        dst[i].z = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
        dst[i].w = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
    }
}

/* The host's sweep (gGfxSweep): a draw of the other kind of geometry starts a new batch. */
static COLD void sweep_vertices(const Vertex *src) {
    int level = port_sweep_level_vertex(src) != 0;
    if (level != gGfxSweepLevel) {
        flush_batch();
        gGfxSweepLevel = level;
    }
}

/*
 * Places the billboard (sprite) vertices just loaded (count of them, from RSP vertex dest) next to their anchor,
 * g.verts[0], and returns their flatness. Their coordinates are offsets from the anchor in clip space, sized for
 * the N64's camera; on the port's wider (G3) or narrower (the widescreen style's field of view) camera they are scaled
 * to keep their size next to the 3D world. Their matrix has no perspective, but they belong with their anchor: the 3D
 * world's, or, anchored by the HUD's or a menu's orthographic matrix (the Save Options balloons, the banana counter),
 * the flat 4:3 picture, where they never stretch to a wide screen's edge as they pass the N64 one (GFX_FLAT_PLACED).
 * Their depth offsets too are sized for the N64's camera, whose near plane is at 10: widescreen's nearer one (2.5,
 * plat_camera_near) maps the same clip-space offset to 4 times the depth, which stood Player Select's bushes in front
 * of the log they grow behind. So the offset past the anchor's depth (the depth offset less the matrix's z scale,
 * about 1, that comes with the vertex's extra w of 1) is scaled by the near plane / 10.
 * A load from vertex 0 brings a new anchor (its first vertex, flat as its matrix is: anchorFlat), which places the
 * ones after it.
 */
static NOINLINE u8 place_billboard(RspVertex *verts, s32 count, s32 dest, u8 anchorFlat) {
    const RspVertex *anchor = &g.verts[0];
    float scaleX = 1.0f, scaleY = 1.0f, scaleZ = 1.0f;
    u8 flat = (dest == 0 ? anchorFlat : anchor->flat) ? GFX_FLAT_PLACED : 0;
    s32 i;
    if (!flat) {
        scaleY = 1.0f / plat_fov_tan_scale();
        scaleX = scaleY / plat_aspect_scale();
        scaleZ = plat_camera_near() / 10.0f;
    }
    for (i = (dest == 0) ? 1 : 0; i < count; i++) {
        RspVertex *v = &verts[i];
        v->x = v->x * scaleX + anchor->x;
        v->y = v->y * scaleY + anchor->y;
        v->z = (v->z - 1.0f) * scaleZ + 1.0f + anchor->z;
        v->w += anchor->w;
    }
    return flat;
}

/* Transforms the vertices into g.verts, with the matrix m and the fog and billboard settings they were loaded with. */
static NOINLINE HOT void load_vertices_now(const Vertex *src, s32 count, s32 dest, float (*m)[4], s32 fog,
                                           s32 billboard) {
    RspVertex *verts = &g.verts[dest];
    s32 i, n = MIN(count, MAX_VERTICES - dest);
    // An orthographic matrix (the HUD's, menus', fades') gives every vertex the same w.
    u8 flat = m[0][3] == 0.0f && m[1][3] == 0.0f && m[2][3] == 0.0f;
    u8 restFlat = flat; // Of the vertices after the first, which may be a billboard's anchor (place_billboard)
    float fogMul = g.fogMul, fogOffset = g.fogOffset;
    u32 fogFlat;
    u64 loaded;

    if (n <= 0) {
        return;
    }
#ifdef HAVE_VFP_VECTORS
    if (sVfpVectors) {
        transform_vertices_vfp(src, n, verts, m);
    } else
#endif
    {
        transform_vertices(src, n, verts, m);
    }
    if (UNLIKELY(billboard)) {
        restFlat = place_billboard(verts, n, dest, flat);
    }
    // The fog factor costs a divide, so it is only worked out for vertices loaded with fog on (all fogged geometry,
    // in practice); draw_dkr_triangles catches any others if fog is turned on before they are drawn.
    fogFlat = (fog ? 0x100u : 0u) | ((u32) restFlat << 16);
    if (!fog) {
        g.vertsWithoutFog = TRUE;
    }
    for (i = 0; i < n; i++) {
        RspVertex *v = &verts[i];
        u32 word = fogFlat;
        __builtin_memcpy(&v->rgba, &src[i].r, 4);
        if (fog) {
            word |= fog_factor(v, fogMul, fogOffset);
        }
        v->fogFlat = word;
    }
    if (dest == 0) {
        verts[0].flat = flat;
    }
    // Which of the RSP's vertices may be flat, for DKR_WALK_LEAN (kept up either way, so that it is right when the
    // switch is turned on).
    loaded = ((n >= 64) ? ~0ULL : ((1ULL << n) - 1)) << dest;
    if (flat | restFlat) {
        g.flatVerts |= loaded;
    } else {
        g.flatVerts &= ~loaded;
    }
}

/*
 * GPU vertex maths (roadmap item P30, DKR_GPU_TRANSFORM, when the backend says it can: gfxb_gpu_transform). A vertex
 * load is only noted (a PendingLoad) instead of transformed. If the triangles that follow use only the vertices of one
 * such load, they go to the backend as the game's own vertices with the load's matrix (model_triangles), and the GPU
 * transforms them. Anything else first transforms the vertices it needs here, from the load they came from
 * (resolve_slots), so that path draws exactly what it did. Each of the RSP's vertex slots remembers which load it
 * still waits for (sSlotLoad); a load whose slots have all been overwritten is never transformed at all. Billboards
 * and flat (orthographic) loads are never deferred.
 */
static s32 sGpuTransform; // This frame's gfxb_gpu_transform

#define PENDING_LOADS 8
#define NO_LOAD 0xFF

typedef struct PendingLoad {
    const Vertex *src;
    s32 dest, count;
    float m[4][4];
    s16 fogMul, fogOffset;
    u8 fog;    // G_FOG was set
    u8 slots;  // Slots still waiting for it
    u8 boxed;  // box holds the vertices' box
    s16 boxMin[3], boxMax[3];
} PendingLoad;
static PendingLoad sLoads[PENDING_LOADS];
static u32 sNextLoad;
static u8 sSlotLoad[MAX_VERTICES]; // The load each slot waits for, or NO_LOAD
static s32 sAnyPending;

/* Transforms the slots still waiting for load l (as load_vertices did when it came), in runs of neighbours. */
static COLD void resolve_load(u32 l) {
    PendingLoad *pl = &sLoads[l];
    s16 fogMul = g.fogMul, fogOffset = g.fogOffset;
    s32 i = pl->dest, end = pl->dest + pl->count;
    g.fogMul = pl->fogMul;
    g.fogOffset = pl->fogOffset;
    while (i < end) {
        s32 run;
        if (sSlotLoad[i] != l) {
            i++;
            continue;
        }
        for (run = i; run < end && sSlotLoad[run] == l; run++) {
            sSlotLoad[run] = NO_LOAD;
        }
        load_vertices_now(pl->src + (i - pl->dest), run - i, i, pl->m, pl->fog, FALSE);
        i = run;
    }
    pl->slots = 0;
    g.fogMul = fogMul;
    g.fogOffset = fogOffset;
}

/* Makes sure slots first to end - 1 hold transformed vertices. */
static void resolve_slots(s32 first, s32 end) {
    s32 i;
    for (i = first; i < end; i++) {
        if (sSlotLoad[i] != NO_LOAD) {
            resolve_load(sSlotLoad[i]);
        }
    }
}

/* The slots first to end - 1 are being loaded again: they no longer wait for their old loads. */
static void forget_slots(s32 first, s32 end) {
    s32 i;
    for (i = first; i < end; i++) {
        if (sSlotLoad[i] != NO_LOAD) {
            sLoads[sSlotLoad[i]].slots--;
            sSlotLoad[i] = NO_LOAD;
        }
    }
}

static void forget_all_loads(void) {
    s32 i;
    for (i = 0; i < MAX_VERTICES; i++) {
        sSlotLoad[i] = NO_LOAD;
    }
    for (i = 0; i < PENDING_LOADS; i++) {
        sLoads[i].slots = 0;
    }
    sAnyPending = FALSE;
}

static NOINLINE HOT void load_vertices(const Vertex *src, s32 count, s32 dest) {
    float(*m)[4] = g.matrices[g.curMatrix];
    s32 fog = (g.geometryMode & G_FOG) != 0, end = MIN(dest + count, MAX_VERTICES);

    if (UNLIKELY(gGfxSweep)) {
        sweep_vertices(src);
    }
    if (sAnyPending) {
        if (g.billboard && dest > 0) {
            resolve_slots(0, 1); // The billboard's anchor
        }
        forget_slots(dest, end);
    }
    if (sGpuTransform && !g.billboard && !gGfxSweep && count > 0 && dest + count <= MAX_VERTICES &&
        !(m[0][3] == 0.0f && m[1][3] == 0.0f && m[2][3] == 0.0f)) {
        u32 l = sNextLoad++ % PENDING_LOADS, i;
        PendingLoad *pl = &sLoads[l];
        if (pl->slots > 0) {
            resolve_load(l); // An old load still waited for: transformed now, as the record is needed
        }
        pl->src = src;
        pl->dest = dest;
        pl->count = count;
        __builtin_memcpy(pl->m, m, sizeof(pl->m));
        pl->fogMul = g.fogMul;
        pl->fogOffset = g.fogOffset;
        pl->fog = fog;
        pl->boxed = FALSE;
        pl->slots = count;
        for (i = dest; i < (u32) end; i++) {
            sSlotLoad[i] = l;
        }
        // Deferred loads have perspective, so none of their vertices is flat (see load_vertices_now, DKR_WALK_LEAN).
        g.flatVerts &= ~(((end - dest >= 64) ? ~0ULL : ((1ULL << (end - dest)) - 1)) << dest);
        sAnyPending = TRUE;
        return;
    }
    load_vertices_now(src, count, dest, m, fog, g.billboard);
}

/* Folds the N64 viewport into clip space coordinates (see gfx_backend.h), with the factors worked out once per draw. */
typedef struct Viewport {
    float scaleX, transX, scaleY, transY;
} Viewport;

static void viewport_init(Viewport *vp) {
    const float halfW = GFX_SCREEN_WIDTH / 2.0f, halfH = GFX_SCREEN_HEIGHT / 2.0f;
    vp->scaleX = g.vpScale[0] / halfW;
    vp->transX = g.vpTrans[0] / halfW - 1.0f;
    vp->scaleY = g.vpScale[1] / halfH;
    vp->transY = 1.0f - g.vpTrans[1] / halfH;
}

/* Vertices loaded with fog off get their fog factor now that fog is on (rare: load_vertices does the rest). */
static NOINLINE void fog_late_vertices(void) {
    s32 i;
    for (i = 0; i < MAX_VERTICES; i++) {
        if (!g.verts[i].fogValid) {
            vertex_fog(&g.verts[i]);
            g.verts[i].fogValid = TRUE;
        }
    }
    g.vertsWithoutFog = FALSE;
}

/*
 * The batch vertices made so far for each of the RSP's vertices in the triangle command being drawn: a DKR triangle
 * carries its own texture coordinates for each corner, so a vertex is shared only between corners with the same
 * ones. In the game's models about half the corners are such repeats, nearly always among the last few uses.
 * The entries are only valid for the batch generation they were made in (sBatchGeneration): the counts are cleared
 * when a triangle command starts and again when its batch is flushed (shared_vertices_clear), which costs less than
 * a generation check at every corner.
 */
#define SHARED_PER_VERTEX 4
typedef struct SharedVertex {
    u32 key[SHARED_PER_VERTEX]; // Texture coordinates, as the triangle holds them
    u16 index[SHARED_PER_VERTEX];
} SharedVertex;
/* One object, so that the corner loop reaches the entries and the counts from one base register. */
static struct {
    SharedVertex entries[MAX_VERTICES];
    u32 countWords[MAX_VERTICES / 4]; // The counts (a byte each), as words for clearing
} sShared;
#define sSharedCount ((u8 *) sShared.countWords)

/* Word stores rather than a library memset (see draw_states_equal); out of line, so that there is one copy. */
static NOINLINE HOT void shared_vertices_clear(void) {
    u32 i;
#pragma GCC unroll 16
    for (i = 0; i < ARRAY_COUNT(sShared.countWords); i++) {
        sShared.countWords[i] = 0;
    }
}

/* Whether a flat triangle reaches both sides of the N64 screen (a full screen picture), as gfx_flat_x sees them. */
static NOINLINE s32 flat_triangle_spans_screen(const u8 idx[3], const Viewport *vp) {
    s32 k, left = FALSE, right = FALSE;
    for (k = 0; k < 3; k++) {
        const RspVertex *v = &g.verts[idx[k]];
        float x = v->x * vp->scaleX + v->w * vp->transX, edge = 0.99f * (v->w < 0.0f ? -v->w : v->w);
        left |= x <= -edge;
        right |= x >= edge;
    }
    return left && right;
}

/******************************** GPU vertex maths (P30) and meshes (P31, P34) ********************************/

/*
 * The batch of model triangles waiting to be drawn (gfxb_draw_model): like sBatchVerts, but the game's own vertices,
 * one per vertex and texture coordinates used, with the matrix and texture factors they share (sModelParams). Only
 * one of the two batches holds triangles at a time, so the draws keep their order.
 */
static GfxModelVertex sModelVerts[MAX_BATCH_VERTICES];
static u16 sModelIndices[MAX_BATCH_TRIANGLES * 3];
static s32 sModelVertices;
static GfxModelParams sModelParams;

#ifdef PORT_GFX_REFERENCE
void refb_draw_model(const GfxDrawState *state, const GfxModelParams *params, const GfxModelVertex *verts, int numVerts,
                     const unsigned short *indices, int numTriangles);
#endif

static COLD void flush_model_batch(void) {
    u32 part;
    if (sModelCount == 0) {
        return;
    }
    if (sFrameCount == sTraceFrame) {
        trace_state(&sModelState, sModelCount);
        plat_log("    model batch, %d vertices", (int) sModelVertices);
    }
    part = walk_switch(WALK_DRAWS);
    gfxb_draw_model(&sModelState, &sModelParams, sModelVerts, sModelVertices, sModelIndices, sModelCount);
    prof_count(PROF_TRIANGLES, sModelCount);
    prof_count(PROF_DRAWS, 1);
    REF_CALL(refb_draw_model(&sModelState, &sModelParams, sModelVerts, sModelVertices, sModelIndices, sModelCount));
    walk_switch(part);
    sModelCount = 0;
    sModelVertices = 0;
    sBatchGeneration++;
}

/* Starts a triangle in the model batch of this state and these parameters (flushing whatever batch is in the way) and
 * returns where its indices go. */
static u16 *model_triangle(const GfxDrawState *state, const GfxModelParams *params) {
    if (sBatchCount > 0 || sMeshPartCount > 0) {
        flush_batch();
    }
    if (sModelCount > 0 && (sModelCount >= MAX_BATCH_TRIANGLES || sModelVertices + 3 > MAX_BATCH_VERTICES ||
                            !draw_states_equal(state, &sModelState) ||
                            __builtin_memcmp(params, &sModelParams, sizeof(*params)) != 0)) {
        flush_model_batch();
    }
    if (sModelCount == 0) {
        sModelState = *state;
        sModelParams = *params;
    }
    return &sModelIndices[sModelCount++ * 3];
}

/* A pending load's box in model space. */
static void pending_box(PendingLoad *pl) {
    const Vertex *v = pl->src;
    s32 i, k;
    if (pl->boxed) {
        return;
    }
    pl->boxMin[0] = pl->boxMax[0] = v[0].x;
    pl->boxMin[1] = pl->boxMax[1] = v[0].y;
    pl->boxMin[2] = pl->boxMax[2] = v[0].z;
    for (i = 1; i < pl->count; i++) {
        s16 c[3] = { v[i].x, v[i].y, v[i].z };
        for (k = 0; k < 3; k++) {
            if (c[k] < pl->boxMin[k]) {
                pl->boxMin[k] = c[k];
            }
            if (c[k] > pl->boxMax[k]) {
                pl->boxMax[k] = c[k];
            }
        }
    }
    pl->boxed = TRUE;
}

/* The texture factors of the model parameters for one texture axis (see tex_axis_coord). */
static void tex_axis_factors(const TexAxis *a, float *scale, float *offset) {
    float inv = a->divide ? 1.0f / a->size : 1.0f; // A power-of-two size is already folded in (tex_axis_init)
    *scale = a->scale * inv;
    *offset = -a->offset * inv;
}

/*
 * Meshes (roadmap items P31, DKR_TRACK_MESH, and P34, DKR_MODEL_MESH). A triangle command drawn by the GPU path builds
 * the same model vertices and indices from the same game data every frame: the track's segments never change, and
 * neither do most objects' models. So the first time a command is seen its vertices and indices are copied once to
 * memory the GPU reads (gfxb_mesh_create), and from then on it is drawn from there with the frame's matrix and
 * textures, with no work per vertex. An entry is keyed by the command's triangle and vertex data, and checked each
 * frame against a hash of that data, so a level loaded at the same address, or an animated model that rewrites its
 * vertices, is noticed; a command whose data keeps changing (MESH_DYNAMIC_AFTER times) is no longer recorded.
 * Track geometry is told from the rest by its vertices lying in the level model's segments.
 */
#define MESH_TABLE_SIZE 4096
#define MESH_PROBES 8
#define MESH_MAX_RUNS 8
#define MESH_DYNAMIC_AFTER 3
#define MESH_UNUSED_FRAMES 600
#define MESH_MAX_BYTES (6 * 1024 * 1024)

typedef struct MeshRun {
    u16 firstIndex, numTriangles;
    u8 doubleSided;
} MeshRun;

typedef struct MeshEntry {
    const Triangle *tris;
    const Vertex *src;
    u16 numTris;
    u8 dest, numVerts;
    u32 hash;
    u32 lastFrame;
    void *mesh; // NULL while not recorded
    u32 bytes;
    u8 used, level, changes, dynamic;
    u8 numRuns;
    MeshRun runs[MESH_MAX_RUNS];
} MeshEntry;

static MeshEntry sMeshes[MESH_TABLE_SIZE];
static u32 sMeshBytes;
static s32 sTrackMesh, sModelMesh; // This frame's DKR_TRACK_MESH and DKR_MODEL_MESH
static u32 sMeshStats[4];          // Draws from meshes, meshes made, commands not recorded, data changes

extern LevelModel *gCurrentLevelModel;

static s32 is_level_vertex(const Vertex *v) {
    s32 i;
    if (gCurrentLevelModel == NULL) {
        return FALSE;
    }
    for (i = 0; i < gCurrentLevelModel->numberOfSegments; i++) {
        const LevelModelSegment *segment = &gCurrentLevelModel->segments[i];
        if (v >= segment->vertices && v < segment->vertices + segment->numberOfVertices) {
            return TRUE;
        }
    }
    return FALSE;
}

static u32 mesh_hash(const Triangle *tris, s32 numTris, const Vertex *src, s32 numVerts) {
    const u32 *t = (const u32 *) tris;
    const u16 *v = (const u16 *) src;
    u32 h = 2166136261u ^ (u32) numTris, i, n;
    for (i = 0, n = numTris * (sizeof(Triangle) / 4); i < n; i++) {
        h = (h ^ t[i]) * 16777619u;
    }
    for (i = 0, n = numVerts * (sizeof(Vertex) / 2); i < n; i++) {
        h = (h ^ v[i]) * 16777619u;
    }
    return h;
}

static void mesh_drop(MeshEntry *e) {
    if (e->mesh != NULL) {
        flush_mesh_batch(); // Its parts may be waiting to be drawn
        gfxb_mesh_free(e->mesh);
        sMeshBytes -= e->bytes;
    }
    __builtin_memset(e, 0, sizeof(*e));
}

/* Drops every mesh at once: with meshes off their memory is better left to textures (the switch bench turns them off
 * between races, and an Old 3DS has little linear memory to spare). */
static COLD void mesh_drop_all(void) {
    u32 i;
    for (i = 0; i < MESH_TABLE_SIZE; i++) {
        if (sMeshes[i].used) {
            mesh_drop(&sMeshes[i]);
        }
    }
}

/* Frees the meshes of commands not drawn for a while (a level left, an object gone), a part of the table a frame. */
static void mesh_age(void) {
    static u32 sNext;
    u32 i;
    for (i = 0; i < MESH_TABLE_SIZE / 64; i++, sNext++) {
        MeshEntry *e = &sMeshes[sNext % MESH_TABLE_SIZE];
        if (e->used && (u32) sFrameCount - e->lastFrame > MESH_UNUSED_FRAMES) {
            mesh_drop(e);
        }
    }
}

/* Builds a command's model vertices and indices into sModelVerts/sModelIndices (as the model batch would, sharing
 * corners with the same texture coordinates), with runs of triangles of the same culling. Returns FALSE if it has
 * more runs than an entry holds. */
static s32 mesh_build(const Triangle *tris, s32 count, const PendingLoad *pl, MeshEntry *e, s32 *numVerts) {
    s32 i, k, verts = 0, indices = 0;
    sBatchGeneration++;
    shared_vertices_clear();
    e->numRuns = 0;
    for (i = 0; i < count; i++) {
        const Triangle *t = &tris[i];
        const TexCoords *uv[3] = { &t->uv0, &t->uv1, &t->uv2 };
        u8 idx[3] = { t->vi0, t->vi1, t->vi2 };
        u8 doubleSided = (t->flags & BACKFACE_DRAW) != 0;
        if (e->numRuns == 0 || e->runs[e->numRuns - 1].doubleSided != doubleSided) {
            if (e->numRuns == MESH_MAX_RUNS) {
                return FALSE;
            }
            e->runs[e->numRuns].firstIndex = indices;
            e->runs[e->numRuns].numTriangles = 0;
            e->runs[e->numRuns].doubleSided = doubleSided;
            e->numRuns++;
        }
        e->runs[e->numRuns - 1].numTriangles++;
        for (k = 0; k < 3; k++) {
            SharedVertex *shared = &sShared.entries[idx[k]];
            u32 key = ((u32) (u16) uv[k]->u << 16) | (u16) uv[k]->v;
            const Vertex *s = &pl->src[idx[k] - pl->dest];
            GfxModelVertex *v;
            s32 q, n;
            n = sSharedCount[idx[k]];
            for (q = 0; q < n; q++) {
                if (shared->key[q] == key) {
                    break;
                }
            }
            if (q < n) {
                sModelIndices[indices++] = shared->index[q];
                continue;
            }
            if (n < SHARED_PER_VERTEX) {
                shared->key[n] = key;
                shared->index[n] = (u16) verts;
                sSharedCount[idx[k]] = n + 1;
            }
            v = &sModelVerts[verts];
            v->x = s->x;
            v->y = s->y;
            v->z = s->z;
            v->one = 1;
            v->u = uv[k]->u;
            v->v = uv[k]->v;
            v->r = s->r;
            v->g = s->g;
            v->b = s->b;
            v->a = s->a;
            sModelIndices[indices++] = (u16) verts++;
        }
    }
    *numVerts = verts;
    return TRUE;
}

/*
 * The batch of mesh parts waiting to be drawn: runs of recorded meshes with the same state and parameters (most of a
 * track's commands with one texture), drawn by the backend as one draw.
 */
#define MESH_BATCH_TRIANGLES 4096
static GfxMeshPart sMeshParts[GFX_MAX_MESH_PARTS];
static s32 sMeshTriangles;

static COLD void flush_mesh_batch(void) {
    u32 part;
    if (sMeshPartCount == 0) {
        return;
    }
    if (sFrameCount == sTraceFrame) {
        trace_state(&sModelState, sMeshTriangles);
        plat_log("    mesh batch, %d parts", (int) sMeshPartCount);
    }
    part = walk_switch(WALK_DRAWS);
    gfxb_draw_meshes(&sModelState, &sModelParams, sMeshParts, sMeshPartCount);
    prof_count(PROF_TRIANGLES, sMeshTriangles);
    prof_count(PROF_DRAWS, 1);
    walk_switch(part);
    sMeshPartCount = 0;
    sMeshTriangles = 0;
}

/* Adds triangles of a mesh to the mesh batch (flushing whatever batch is in the way). */
static void mesh_part(const GfxDrawState *state, const GfxModelParams *params, void *mesh, s32 firstIndex,
                      s32 numTriangles) {
    GfxMeshPart *last;
    if (sBatchCount > 0 || sModelCount > 0) {
        flush_batch();
    }
    if (sMeshPartCount > 0 &&
        (sMeshPartCount == GFX_MAX_MESH_PARTS || sMeshTriangles + numTriangles > MESH_BATCH_TRIANGLES ||
         !draw_states_equal(state, &sModelState) || __builtin_memcmp(params, &sModelParams, sizeof(*params)) != 0)) {
        flush_mesh_batch();
    }
    if (sMeshPartCount == 0) {
        sModelState = *state;
        sModelParams = *params;
    }
    last = sMeshPartCount > 0 ? &sMeshParts[sMeshPartCount - 1] : NULL;
    if (last != NULL && last->mesh == mesh && last->firstIndex + last->numTriangles * 3 == firstIndex) {
        last->numTriangles += numTriangles;
    } else {
        last = &sMeshParts[sMeshPartCount++];
        last->mesh = mesh;
        last->firstIndex = firstIndex;
        last->numTriangles = numTriangles;
    }
    sMeshTriangles += numTriangles;
}

/*
 * Draws the command from its mesh, recording the mesh first if this is the first time (or its data changed). Returns
 * FALSE when it isn't to be drawn that way (meshes off for its kind, too big, data that keeps changing, out of room),
 * for the model batch to draw it.
 */
static s32 mesh_triangles(const Triangle *tris, s32 count, const PendingLoad *pl, const GfxDrawState *baseState,
                          const GfxModelParams *params, u8 frontCull) {
    MeshEntry *e = NULL, *freeSlot = NULL;
    u32 slot = ((uintptr_t) tris >> 4) % MESH_TABLE_SIZE, hash, i;
    GfxDrawState state;
    s32 level;

    if (count > MAX_BATCH_TRIANGLES || pl->count > MAX_VERTICES) {
        return FALSE;
    }
    for (i = 0; i < MESH_PROBES; i++) {
        MeshEntry *c = &sMeshes[(slot + i) % MESH_TABLE_SIZE];
        if (c->used && c->tris == tris && c->src == pl->src && c->numTris == count &&
            c->dest == pl->dest && c->numVerts == pl->count) {
            e = c;
            break;
        }
        if (!c->used && freeSlot == NULL) {
            freeSlot = c;
        }
    }
    if (e == NULL) {
        if (freeSlot == NULL) {
            return FALSE;
        }
        level = is_level_vertex(pl->src);
        if (!(level ? sTrackMesh : sModelMesh)) {
            return FALSE; // Not recorded, and not remembered either: the check is cheap.
        }
        e = freeSlot;
        e->used = TRUE;
        e->tris = tris;
        e->src = pl->src;
        e->numTris = count;
        e->dest = pl->dest;
        e->numVerts = pl->count;
        e->level = level;
        e->hash = 0;
    } else if (!(e->level ? sTrackMesh : sModelMesh)) {
        return FALSE;
    }
    e->lastFrame = sFrameCount;
    if (e->dynamic) {
        return FALSE;
    }
    hash = mesh_hash(tris, count, pl->src, pl->count);
    if (e->mesh == NULL || hash != e->hash) {
        s32 numVerts;
        if (e->mesh != NULL) {
            sMeshStats[3]++;
            flush_mesh_batch();
            gfxb_mesh_free(e->mesh);
            sMeshBytes -= e->bytes;
            e->mesh = NULL;
            if (++e->changes >= MESH_DYNAMIC_AFTER) {
                e->dynamic = TRUE;
                sMeshStats[2]++;
                return FALSE;
            }
        }
        if (sModelCount > 0) {
            flush_model_batch(); // mesh_build works in the batch's arrays
        }
        if (!mesh_build(tris, count, pl, e, &numVerts)) {
            e->dynamic = TRUE;
            sMeshStats[2]++;
            return FALSE;
        }
        e->bytes = numVerts * sizeof(GfxModelVertex) + count * 3 * sizeof(u16);
        if (sMeshBytes + e->bytes > MESH_MAX_BYTES ||
            (e->mesh = gfxb_mesh_create(sModelVerts, numVerts, sModelIndices, count * 3)) == NULL) {
            return FALSE; // Out of room: tried again next frame, once old ones may have gone.
        }
        sMeshBytes += e->bytes;
        e->hash = hash;
        sMeshStats[1]++;
    }
    state = *baseState;
    for (i = 0; i < e->numRuns; i++) {
        state.cullMode = e->runs[i].doubleSided ? 0 : frontCull;
        mesh_part(&state, params, e->mesh, e->runs[i].firstIndex, e->runs[i].numTriangles);
        sMeshStats[0]++;
    }
    return TRUE;
}

/*
 * Draws a triangle command with GPU vertex maths, if every corner is one of the pending load's vertices and the
 * backend takes the load's box (else returns FALSE, for the CPU path). Returns TRUE when it is done: drawn, or wholly
 * outside the guard band, which the CPU path would have dropped too.
 */
static COLD s32 model_triangles(const Triangle *tris, s32 count, s32 textured) {
    GfxDrawState state;
    GfxModelParams params;
    Viewport vp;
    PendingLoad *pl;
    s32 i, k, first, verdict;
    u32 generation;
    u8 frontCull = g.vpScale[0] < 0.0f ? 2 : 1, batchedCull = 0xFF, l;

    if (count <= 0 || (l = sSlotLoad[tris[0].vi0]) == NO_LOAD) {
        return FALSE;
    }
    for (i = 0; i < count; i++) {
        if (tris[i].vi0 >= MAX_VERTICES || tris[i].vi1 >= MAX_VERTICES || tris[i].vi2 >= MAX_VERTICES ||
            sSlotLoad[tris[i].vi0] != l || sSlotLoad[tris[i].vi1] != l || sSlotLoad[tris[i].vi2] != l) {
            return FALSE;
        }
    }
    pl = &sLoads[l];
    first = pl->dest;
    viewport_init(&vp);
    for (i = 0; i < 4; i++) {
        float(*m)[4] = pl->m;
        params.mtx[i][0] = m[i][0] * vp.scaleX + m[i][3] * vp.transX;
        params.mtx[i][1] = m[i][1] * vp.scaleY + m[i][3] * vp.transY;
        params.mtx[i][2] = m[i][2];
        params.mtx[i][3] = m[i][3];
    }
    pending_box(pl);
    verdict = gfxb_model_check(params.mtx, pl->boxMin, pl->boxMax);
    if (sFrameCount == sTraceFrame) {
        plat_log("  MODEL %d tris, load %d of %d at %d, box %d %d %d .. %d %d %d: %s", (int) count, (int) l,
                 (int) pl->count, (int) pl->dest, pl->boxMin[0], pl->boxMin[1], pl->boxMin[2], pl->boxMax[0],
                 pl->boxMax[1], pl->boxMax[2],
                 verdict == GFX_MODEL_HIDDEN ? "hidden" : verdict == GFX_MODEL_GPU ? "GPU" : "CPU");
    }
    if (verdict == GFX_MODEL_HIDDEN) {
        return TRUE;
    }
    if (verdict != GFX_MODEL_GPU) {
        return FALSE;
    }

    if (!sTriStateValid) {
        build_draw_state(&sTriState, FALSE);
        sTriStateValid = TRUE;
    }
    state = sTriState;
    if (!textured) {
        state.textures[0] = NULL;
        state.textures[1] = NULL;
    }
    state.flat = 0;
    for (i = 0; i < 4; i++) {
        params.texScale[i] = 0.0f;
        params.texOffset[i] = 0.0f;
    }
    for (i = 0; i < 2; i++) {
        if (state.textures[i] != NULL) {
            Tile *tile = &g.tiles[i];
            float shift = texel_centre_shift(state.textures[i]);
            TexAxis u, v;
            tex_axis_init(&u, tile->shifts, tile->uls, state.textures[i]->width, shift);
            tex_axis_init(&v, tile->shiftt, tile->ult, state.textures[i]->height, shift);
            tex_axis_factors(&u, &params.texScale[i * 2], &params.texOffset[i * 2]);
            tex_axis_factors(&v, &params.texScale[i * 2 + 1], &params.texOffset[i * 2 + 1]);
        }
    }
    params.fog = (g.geometryMode & G_FOG) != 0;
    // Fog worked out when the vertices came uses that time's settings, as load_vertices_now does
    params.fogMul = params.fog ? (pl->fog ? pl->fogMul : g.fogMul) : 0;
    params.fogOffset = params.fog ? (pl->fog ? pl->fogOffset : g.fogOffset) : 0;
    params.pad[0] = params.pad[1] = params.pad[2] = 0;

    if ((sTrackMesh || sModelMesh) && !REF_ACTIVE() && mesh_triangles(tris, count, pl, &state, &params, frontCull)) {
        return TRUE;
    }

    sBatchGeneration++;
    generation = sBatchGeneration;
    shared_vertices_clear();
    for (i = 0; i < count; i++) {
        const Triangle *t = &tris[i];
        const TexCoords *uv[3] = { &t->uv0, &t->uv1, &t->uv2 };
        u8 idx[3] = { t->vi0, t->vi1, t->vi2 };
        u16 *out;

        state.cullMode = (t->flags & BACKFACE_DRAW) ? 0 : frontCull;
        if (state.cullMode == batchedCull && sModelCount > 0 && sModelCount < MAX_BATCH_TRIANGLES &&
            sModelVertices + 3 <= MAX_BATCH_VERTICES) {
            out = &sModelIndices[sModelCount++ * 3];
        } else {
            out = model_triangle(&state, &params);
            batchedCull = state.cullMode;
            if (sBatchGeneration != generation) { // A batch was flushed: its vertices are gone
                generation = sBatchGeneration;
                shared_vertices_clear();
            }
        }
        for (k = 0; k < 3; k++) {
            SharedVertex *shared = &sShared.entries[idx[k]];
            u32 key = ((u32) (u16) uv[k]->u << 16) | (u16) uv[k]->v;
            const Vertex *s = &pl->src[idx[k] - first];
            GfxModelVertex *v;
            s32 q, n;

            n = sSharedCount[idx[k]];
            for (q = 0; q < n; q++) {
                if (shared->key[q] == key) {
                    break;
                }
            }
            if (q < n) {
                out[k] = shared->index[q];
                continue;
            }
            out[k] = (u16) sModelVertices;
            if (n < SHARED_PER_VERTEX) {
                shared->key[n] = key;
                shared->index[n] = out[k];
                sSharedCount[idx[k]] = n + 1;
            }
            v = &sModelVerts[sModelVertices++];
            v->x = s->x;
            v->y = s->y;
            v->z = s->z;
            v->one = 1;
            v->u = uv[k]->u;
            v->v = uv[k]->v;
            v->r = s->r;
            v->g = s->g;
            v->b = s->b;
            v->a = s->a;
        }
    }
    return TRUE;
}

static NOINLINE HOT void draw_dkr_triangles(const Triangle *tris, s32 count, s32 textured) {
    GfxDrawState state;
    s32 i, k, fog = (g.geometryMode & G_FOG) != 0;
    Tile *tile0 = &g.tiles[0], *tile1 = &g.tiles[1];
    TexAxis u0, v0, u1, v1;
    Viewport vp;
    u8 batchedKey = 0xFF; // The batch's culling and flatness
    u8 frontCull = g.vpScale[0] < 0.0f ? 2 : 1;
    s32 tex0, tex1, anyFlat, zeroTex1;
    u32 generation;
    float keepSign; // The sign of a kept triangle's determinant, for the culling below

    if (sAnyPending) {
        if (model_triangles(tris, count, textured)) {
            return;
        }
        for (i = 0; i < count; i++) {
            const Triangle *t = &tris[i];
            if (t->vi0 < MAX_VERTICES && sSlotLoad[t->vi0] != NO_LOAD) {
                resolve_load(sSlotLoad[t->vi0]);
            }
            if (t->vi1 < MAX_VERTICES && sSlotLoad[t->vi1] != NO_LOAD) {
                resolve_load(sSlotLoad[t->vi1]);
            }
            if (t->vi2 < MAX_VERTICES && sSlotLoad[t->vi2] != NO_LOAD) {
                resolve_load(sSlotLoad[t->vi2]);
            }
        }
    }
    if (!sTriStateValid) {
        build_draw_state(&sTriState, FALSE);
        sTriStateValid = TRUE;
    }
    state = sTriState;
    if (UNLIKELY(fog && g.vertsWithoutFog)) {
        fog_late_vertices();
    }
    if (!textured) {
        state.textures[0] = NULL;
        state.textures[1] = NULL;
    }
    viewport_init(&vp);
    // The GPU (and the host's renderer) culls by the sign of a triangle's area on the screen. For the finished
    // vertices' x, y and w (with the viewport folded in, see below) that is the sign of the 3x3 determinant of their
    // (x, y, w) rows wherever all three w are positive, and of the part in front of the camera of one that crosses it
    // (clipping keeps the winding). Folding the viewport scales the determinant by scaleX * scaleY. Culling mode 1
    // drops triangles clockwise on the screen, whose determinant is negative, and mode 2 counter clockwise ones.
    keepSign = (vp.scaleX * vp.scaleY < 0.0f) != (frontCull == 2) ? -1.0f : 1.0f;
    // Texture coordinates are only worked out for the textures the draw uses; the others are left at zero.
    tex0 = state.textures[0] != NULL;
    tex1 = state.textures[1] != NULL;
    if (tex0) {
        const GfxTexture *tex = state.textures[0];
        float shift = texel_centre_shift(tex);
        tex_axis_init(&u0, tile0->shifts, tile0->uls, tex->width, shift);
        tex_axis_init(&v0, tile0->shiftt, tile0->ult, tex->height, shift);
    }
    if (tex1) {
        const GfxTexture *tex = state.textures[1];
        float shift = texel_centre_shift(tex);
        tex_axis_init(&u1, tile1->shifts, tile1->uls, tex->width, shift);
        tex_axis_init(&v1, tile1->shiftt, tile1->ult, tex->height, shift);
    }
    // While no loaded vertex is flat (the 3D world) no triangle is, so DKR_WALK_LEAN skips the checks per triangle.
    anyFlat = !sWalkLean || g.flatVerts != 0;
    // The second texture's coordinates are only read by draws that bind one (gfx_c3d.c's write_vertices, the
    // software renderers), so with DKR_WALK_LEAN the others leave the batch's old, finite values in them.
    zeroTex1 = !tex1 && !sWalkLean;
    // Vertices of earlier commands (or loads) can't be shared: the vertex data, viewport and fog may have changed.
    sBatchGeneration++;
    generation = sBatchGeneration;
    shared_vertices_clear();
    for (i = 0; i < count; i++) {
        const Triangle *t = &tris[i];
        TexCoords uv[3]; // uv0, uv1 and uv2, copied out as the three loads they are (not indexed past uv0)
        uv[0] = t->uv0;
        uv[1] = t->uv1;
        uv[2] = t->uv2;
        const u8 *idx = &t->verticesArray[1]; // vi0, vi1 and vi2
        u8 flat = 0, batchKey;
        u16 *out;

        if (idx[0] >= MAX_VERTICES || idx[1] >= MAX_VERTICES || idx[2] >= MAX_VERTICES) {
            continue;
        }
        // Backface culling: flag 0x40 means double sided. The game mirrors the viewport for mirror mode.
        state.cullMode = (t->flags & BACKFACE_DRAW) ? 0 : frontCull;
        if (anyFlat) {
            u8 flat0 = g.verts[idx[0]].flat, flat1 = g.verts[idx[1]].flat, flat2 = g.verts[idx[2]].flat;
            flat = (flat0 && flat1 && flat2) ? (flat0 | flat1 | flat2) : 0; // 1, or GFX_FLAT_PLACED (3) if a sprite
            if (flat == 1 && tex0 && !flat_triangle_spans_screen(idx, &vp)) {
                // Textured and narrower than the screen (a menu's wooden box): on a wide screen it keeps its shape
                // as it slides past the N64 edge, rather than a corner near the edge snapping out to the wide one's.
                flat = GFX_FLAT_PLACED;
            }
        }
        state.flat = flat;
        if (sCpuCull && state.cullMode != 0 && !flat) {
            // Flat draws keep the GPU's culling: on a wide screen gfx_flat_x moves their corners unevenly.
            const RspVertex *a = &g.verts[idx[0]], *b = &g.verts[idx[1]], *c = &g.verts[idx[2]];
            float det = a->x * (b->y * c->w - c->y * b->w) - a->y * (b->x * c->w - c->x * b->w) +
                        a->w * (b->x * c->y - c->x * b->y);
            if (!(det * keepSign > 0.0f)) {
                continue; // A back face, or edge on (no area: nothing would be drawn)
            }
            state.cullMode = 0;
        }
        batchKey = state.cullMode | (flat << 2);
        if (batchKey == batchedKey && sBatchCount > 0 && batch_has_room()) {
            // Only the culling and flatness change from one triangle to the next, so the batch's state is still this
            // one.
            out = &sBatchIndices[sBatchCount++ * 3];
        } else {
            out = batch_triangle(&state);
            batchedKey = batchKey;
            if (sBatchGeneration != generation) {
                // The batch was flushed: its vertices are gone.
                generation = sBatchGeneration;
                shared_vertices_clear();
            }
        }
        for (k = 0; k < 3; k++) {
            SharedVertex *shared = &sShared.entries[idx[k]];
            u32 key = uv[k].texCoords; // Both coordinates, as the table's key
            const RspVertex *in;
            GfxVertex *v;
            s32 q, n;
            u32 rgba;
            u16 index;

            n = sSharedCount[idx[k]];
            for (q = 0; q < n; q++) {
                if (shared->key[q] == key) {
                    break;
                }
            }
            if (q < n) {
                out[k] = shared->index[q];
                continue;
            }
            index = (u16) sBatchVertices++;
            v = &sBatchVerts[index];
            out[k] = index;
            if (n < SHARED_PER_VERTEX) {
                shared->key[n] = key;
                shared->index[n] = index;
                sSharedCount[idx[k]] = n + 1;
            }
            // The finished vertex: the N64 viewport folded in, and with G_FOG set the RSP replaces the shade alpha
            // with the fog factor.
            in = &g.verts[idx[k]];
            v->x = in->x * vp.scaleX + in->w * vp.transX;
            v->y = in->y * vp.scaleY + in->w * vp.transY;
            v->z = in->z;
            v->w = in->w;
            rgba = in->rgba;
            if (fog) {
                rgba = (rgba & 0x00FFFFFFu) | (in->fogFlat << 24);
            }
            __builtin_memcpy(&v->r, &rgba, 4);
            if (tex0) {
                v->u0 = tex_axis_coord(&u0, uv[k].u);
                v->v0 = tex_axis_coord(&v0, uv[k].v);
            } else {
                v->u0 = v->v0 = 0.0f;
            }
            if (tex1) {
                v->u1 = tex_axis_coord(&u1, uv[k].u);
                v->v1 = tex_axis_coord(&v1, uv[k].v);
            } else if (zeroTex1) {
                v->u1 = v->v1 = 0.0f;
            }
        }
    }
}

/******************************** RDP rectangles ********************************/

static void screen_to_clip(GfxVertex *v, float sx, float sy) {
    v->x = sx / (GFX_SCREEN_WIDTH / 2.0f) - 1.0f;
    v->y = 1.0f - sy / (GFX_SCREEN_HEIGHT / 2.0f);
    v->z = -1.0f;
    v->w = 1.0f;
}

static void draw_rect(float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1, s32 flip,
                      s32 textured) {
    GfxDrawState state;
    GfxVertex corners[4];
    u16 first[4], *out;
    s32 i;
    float w0 = 1.0f, h0 = 1.0f, w1 = 1.0f, h1 = 1.0f, shift0 = 0.0f, shift1 = 0.0f;
    Tile *tile = &g.tiles[g.rdpHalf1 & 7];
    Tile *tile1 = &g.tiles[(g.rdpHalf1 + 1) & 7];

    build_draw_state(&state, TRUE);
    if (!textured) {
        state.textures[0] = NULL;
        state.textures[1] = NULL;
    } else if (x0 > 1.0f || x1 < GFX_SCREEN_WIDTH - 1.0f) {
        // Narrower than the screen: on a wide screen it keeps its size even where it ends at an edge (gfx_flat_x).
        state.flat = GFX_FLAT_PLACED;
    }
    if (state.textures[0] != NULL) {
        w0 = state.textures[0]->width;
        h0 = state.textures[0]->height;
        shift0 = texel_centre_shift(state.textures[0]);
    }
    if (state.textures[1] != NULL) {
        w1 = state.textures[1]->width;
        h1 = state.textures[1]->height;
        shift1 = texel_centre_shift(state.textures[1]);
    }
    screen_to_clip(&corners[0], x0, y0);
    screen_to_clip(&corners[1], x1, y0);
    screen_to_clip(&corners[2], x0, y1);
    screen_to_clip(&corners[3], x1, y1);
    for (i = 0; i < 4; i++) {
        float s = (i & 1) ? s1 : s0;
        float t = (i & 2) ? t1 : t0;
        if (flip) {
            s = (i & 2) ? s1 : s0;
            t = (i & 1) ? t1 : t0;
        }
        corners[i].u0 = (s - tile->uls * 0.25f + shift0) / w0;
        corners[i].v0 = (t - tile->ult * 0.25f + shift0) / h0;
        corners[i].u1 = (s - tile1->uls * 0.25f + shift1) / w1;
        corners[i].v1 = (t - tile1->ult * 0.25f + shift1) / h1;
        corners[i].r = corners[i].g = corners[i].b = corners[i].a = 255;
    }
    state.cullMode = 0;
    batch_reserve(&state, 2, 4);
    for (i = 0; i < 4; i++) {
        GfxVertex *v;
        first[i] = batch_vertex(&v);
        *v = corners[i];
    }
    out = &sBatchIndices[sBatchCount++ * 3];
    out[0] = first[0];
    out[1] = first[2];
    out[2] = first[1];
    out = &sBatchIndices[sBatchCount++ * 3];
    out[0] = first[1];
    out[1] = first[2];
    out[2] = first[3];
}

/* A rectangle's x (10.2 fixed point), read as signed: the game's x past the left edge wraps to 1023 and down, and the
 * port has it draw there (src/rcp_dkr.c, src/font.c), where the wide screen shows it. On the N64 screen both are off
 * the screen, and nothing the game draws starts 512 pixels or more to the right. */
static inline float rect_x(u32 field) {
    return (s32) ((field & 0xFFF) ^ 0x800) - 0x800;
}

static NOINLINE void texture_rectangle(u32 w0, u32 w1, s32 flip) {
    float ulx = rect_x(w1 >> 12) * 0.25f;
    // The width, not the right edge, read as is: a rectangle across 512 pixels (the HUD sliding in from the right)
    // would otherwise turn around and cover the screen.
    float lrx = ulx + (((w0 >> 12) - (w1 >> 12)) & 0xFFF) * 0.25f;
    float lry = (w0 & 0xFFF) * 0.25f;
    float uly = (w1 & 0xFFF) * 0.25f;
    s32 tile = (w1 >> 24) & 7;
    float s = (s16) (g.rdpHalf1 >> 16) * (1.0f / 32.0f);
    float t = (s16) (g.rdpHalf1 & 0xFFFF) * (1.0f / 32.0f);
    float dsdx = (s16) (g.rdpHalf2 >> 16) * (1.0f / 1024.0f);
    float dtdy = (s16) (g.rdpHalf2 & 0xFFFF) * (1.0f / 1024.0f);
    float s1, t1;

    if (cycle_type() == G_CYC_COPY) {
        // Copy mode draws 4 pixels per step along x and includes the last column/row.
        if (flip) {
            dtdy *= 0.25f;
        } else {
            dsdx *= 0.25f;
        }
        lrx += 1.0f;
        lry += 1.0f;
    }
    if (flip) {
        // Flipped, s steps down the rectangle (by DsDx) and t across it (by DtDy).
        s1 = s + dsdx * (lry - uly);
        t1 = t + dtdy * (lrx - ulx);
    } else {
        s1 = s + dsdx * (lrx - ulx);
        t1 = t + dtdy * (lry - uly);
    }
    // draw_rect looks the tiles up through rdpHalf1, so stash the tile number there.
    g.rdpHalf1 = tile;
    draw_rect(ulx, uly, lrx, lry, s, t, s1, t1, flip, TRUE);
}

static NOINLINE void fill_rectangle(u32 w0, u32 w1) {
    s32 lrx = (w0 >> 14) & 0x3FF, lry = (w0 >> 2) & 0x3FF;
    s32 ulx = (w1 >> 14) & 0x3FF, uly = (w1 >> 2) & 0x3FF;
    s16 rect[4];
    u32 cyc = cycle_type();

    if (cyc == G_CYC_FILL || cyc == G_CYC_COPY) {
        lrx++;
        lry++;
    }
    rect[0] = ulx;
    rect[1] = uly;
    rect[2] = lrx;
    rect[3] = lry;
    // Clip to the scissor. In fill and copy modes the RDP includes the scissor's right edge column, and DKR relies on
    // that: it clears the screen with the scissor at 319, then draws the scene with it at 320.
    if (rect[0] < g.scissor[0]) rect[0] = g.scissor[0];
    if (rect[1] < g.scissor[1]) rect[1] = g.scissor[1];
    if (rect[2] > scissor_right(cyc)) rect[2] = scissor_right(cyc);
    if (rect[3] > g.scissor[3]) rect[3] = g.scissor[3];
    if (rect[2] <= rect[0] || rect[3] <= rect[1]) {
        return;
    }
    if (cyc == G_CYC_FILL) {
        u32 part;
        flush_batch();
        part = walk_switch(WALK_DRAWS);
        if (g.colorImage == g.depthImage) {
            gfx_split_clear_depth(rect);
            REF_CALL(refb_clear_depth(rect));
        } else {
            u32 c = rgba5551_to_8888(g.fillColor >> 16);
            gfx_split_clear_color(rect, c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 255);
            REF_CALL(refb_clear_color(rect, c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 255));
        }
        walk_switch(part);
        return;
    }
    if (g.colorImage == g.depthImage) {
        return;
    }
    g.rdpHalf1 = 0;
    draw_rect(rect[0], rect[1], rect[2], rect[3], 0, 0, 0, 0, FALSE, FALSE);
}

/******************************** Display list ********************************/

/* F3D defines its immediate opcodes as negative numbers. */
#define OP(x) ((u8) (x))

static void set_other_mode(u32 *mode, u32 shift, u32 len, u32 data) {
    u32 mask = ((1u << len) - 1) << shift;
    *mode = (*mode & ~mask) | (data & mask);
}

static void run_dl(const Gfx *dl, s32 maxCommands);
static void run_command_list(const Gfx *dl, s32 maxCommands, s32 depth);

/*
 * Per opcode: the walk part its time goes to (bits 0-2, WALK_COMMANDS unless listed) and whether it leaves what
 * build_draw_state builds alone (OP_KEEPS_STATE), so the walk reads one byte a command instead of running two
 * switches. Viewport and matrix changes don't change the draw state (culling is set per triangle), and of
 * G_MOVEWORD only G_MW_FOG does, which its case handles.
 */
#define OP_KEEPS_STATE 8
static const u8 sOpInfo[256] = {
    [OP(G_SPNOOP)] = OP_KEEPS_STATE,
    [OP(G_NOOP)] = OP_KEEPS_STATE,
    [OP(G_RDPLOADSYNC)] = OP_KEEPS_STATE,
    [OP(G_RDPPIPESYNC)] = OP_KEEPS_STATE,
    [OP(G_RDPTILESYNC)] = OP_KEEPS_STATE,
    [OP(G_RDPFULLSYNC)] = OP_KEEPS_STATE,
    [OP(G_MOVEMEM)] = OP_KEEPS_STATE,
    [OP(G_POPMTX)] = OP_KEEPS_STATE,
    [OP(G_DL)] = OP_KEEPS_STATE,
    [OP(G_DMADL)] = OP_KEEPS_STATE,
    [OP(G_ENDDL)] = OP_KEEPS_STATE,
    [OP(G_MOVEWORD)] = OP_KEEPS_STATE,
    [OP(G_MTX)] = OP_KEEPS_STATE | WALK_MATRICES,
    [OP(G_VTX)] = OP_KEEPS_STATE | WALK_VERTICES,
    [OP(G_TRIN)] = OP_KEEPS_STATE | WALK_TRIANGLES,
    [OP(G_TEXRECT)] = WALK_RECTS,
    [OP(G_TEXRECTFLIP)] = WALK_RECTS,
    [OP(G_FILLRECT)] = WALK_RECTS,
};

/* The traced frame's log of a command. */
static COLD void trace_command(s32 depth, u32 w0, u32 w1) {
    plat_log("%*s%08X %08X", (int) (depth * 2), "", (unsigned) w0, (unsigned) w1);
}

/*
 * Whether a display list call's target can't be a display list: not word aligned, or in the unmapped first 64 KB. The
 * N64 masks such an address and reads whatever is there; here it would be a crash (bug reports #178 and #181, both on
 * the first frame after a menu level load). The list holding the call is garbage too, so the caller stops reading it.
 */
static inline s32 bad_list_address(u32 addr) {
    return (addr & 3) != 0 || addr < 0x10000;
}

static COLD void log_bad_list_address(u32 addr, const Gfx *where) {
    static s32 sLastLogged = -1;
    if (sLastLogged != sFrameCount) {
        sLastLogged = sFrameCount;
        plat_log("renderer: skipped a display list at bad address %08X, called from %p (%08X %08X), frame %d",
                 (unsigned) addr, (const void *) where, (unsigned) where->words.w0, (unsigned) where->words.w1,
                 (int) sFrameCount);
    }
}

/*
 * G_DL and G_DMADL: runs the called list (up to DL_STACK_SIZE deep) and returns whether the calling list is done
 * (G_DL without a push, or a call to an address that can't be a list). Out of line so that the command loop holds
 * no test of the depth, which never changes within it: the compiler would otherwise unswitch the loop on it, into
 * two copies of the whole loop.
 */
static NOINLINE HOT s32 call_display_list(u32 w0, u32 w1, const Gfx *where, s32 depth) {
    u32 op = w0 >> 24;
    if (UNLIKELY(bad_list_address(w1))) {
        log_bad_list_address(w1, where);
        return TRUE;
    }
    if (depth < DL_STACK_SIZE) {
        s32 maxCommands = (op == OP(G_DMADL)) ? (s32) ((w0 >> 16) & 0xFF) : -1;
        run_command_list((const Gfx *) seg_addr(w1), maxCommands, depth + 1);
    }
    return op == OP(G_DL) && ((w0 >> 16) & G_DL_NOPUSH);
}

/* A load at TMEM word tmem of bytes bytes overwrote the later 64 byte slots it covers: earlier loads there are gone.
 * Most of them are already gone, so only the valid ones (g.tmemValid) are visited. The load itself is marked valid. */
static HOT void forget_covered_loads(u32 tmem, u32 bytes) {
    u32 first = (tmem / 8) & 63, end = (tmem + (bytes + 7) / 8 + 7) / 8;
    u64 covered, m;
    if (end > 64) {
        end = 64;
    }
    covered = (end > first + 1) ? ((end >= 64 ? ~0ULL : (1ULL << end) - 1) & ~((2ULL << first) - 1)) : 0;
    for (m = g.tmemValid & covered; m != 0; m &= m - 1) {
        g.tmem[__builtin_ctzll(m)].valid = FALSE;
    }
    g.tmemValid = (g.tmemValid & ~covered) | (1ULL << first);
}

static HOT void run_command_list(const Gfx *dl, s32 maxCommands, s32 depth) {
    // Commands left to run: a G_DMADL's count, or (no list is that long) as good as no limit, so that the loop tests
    // one count rather than the limit and the count.
    s32 remaining = (maxCommands < 0) ? 0x7FFFFFFF : maxCommands;

    while (remaining > 0) {
        u32 w0 = dl->words.w0;
        u32 w1 = dl->words.w1;
        u32 op = w0 >> 24;
        u32 info = sOpInfo[op];
        dl++;
        remaining--;
        if (UNLIKELY(sTracing)) {
            trace_command(depth, w0, w1);
        }
        if (sWalkTimed && (info & 7) != sWalkPart) {
            walk_switch_timed(info & 7);
        }
        if (!(info & OP_KEEPS_STATE)) {
            sTriStateValid = FALSE;
        }

        switch (op) {
            case OP(G_NOOP):
                if ((w1 & 0xFFFF0000) == PORT_DRAW_TAG) {
                    sDrawKind = w1 & 3;
                }
                break;
            case OP(G_SPNOOP):
            case OP(G_RDPLOADSYNC):
            case OP(G_RDPPIPESYNC):
            case OP(G_RDPTILESYNC):
            case OP(G_RDPFULLSYNC):
            case OP(G_SETKEYGB):
            case OP(G_SETKEYR):
            case OP(G_SETCONVERT):
            case OP(G_SETPRIMDEPTH):
            case OP(G_CULLDL):
                break;

            case OP(G_MTX):
                load_matrix((const Mtx *) seg_addr(w1), (w0 >> 22) & 3);
                g.curMatrix = (w0 >> 22) & 3;
                break;

            case OP(G_MOVEMEM):
                if (((w0 >> 16) & 0xFF) == G_MV_VIEWPORT) {
                    const Vp *vp = (const Vp *) seg_addr(w1);
                    s32 i;
                    for (i = 0; i < 3; i++) {
                        g.vpScale[i] = vp->vp.vscale[i] * 0.25f;
                        g.vpTrans[i] = vp->vp.vtrans[i] * 0.25f;
                    }
                }
                break;

            case OP(G_VTX): {
                s32 count = ((w0 >> 19) & 0x1F) + 1;
                if (w0 & 0x00010000) {
                    if (g.billboard) {
                        g.vertexIndex = 1;
                    }
                } else {
                    g.vertexIndex = 0;
                }
                load_vertices((const Vertex *) seg_addr(w1), count, g.vertexIndex);
                g.vertexIndex += count;
                sDrawMix[sDrawKind][2] += count;
                break;
            }

            case OP(G_TRIN):
                draw_dkr_triangles((const Triangle *) seg_addr(w1), (w0 >> 4) & 0xFFF, (w0 >> 16) & 1);
                g.vertexIndex = 0;
                break;

            case OP(G_DL):
            case OP(G_DMADL):
                if (call_display_list(w0, w1, dl - 1, depth)) {
                    return;
                }
                break;

            case OP(G_ENDDL):
                return;

            case OP(G_SETGEOMETRYMODE):
                g.geometryMode |= w1;
                break;

            case OP(G_CLEARGEOMETRYMODE):
                g.geometryMode &= ~w1;
                break;

            case OP(G_SETOTHERMODE_H):
                set_other_mode(&g.omH, (w0 >> 8) & 0xFF, w0 & 0xFF, w1);
                break;

            case OP(G_SETOTHERMODE_L):
                set_other_mode(&g.omL, (w0 >> 8) & 0xFF, w0 & 0xFF, w1);
                break;

            case OP(G_RDPSETOTHERMODE):
                g.omH = w0 & 0x00FFFFFF;
                g.omL = w1;
                break;

            case OP(G_TEXTURE):
                break;

            case OP(G_MOVEWORD):
                switch (w0 & 0xFF) {
                    case OP(G_MW_BILLBOARD):
                        g.billboard = w1 & 1;
                        break;
                    case OP(G_MW_MVPMATRIX):
                        g.curMatrix = (w1 >> 6) & 3;
                        break;
                    case OP(G_MW_FOG):
                        sTriStateValid = FALSE;
                        g.fogMul = (s16) (w1 >> 16);
                        g.fogOffset = (s16) (w1 & 0xFFFF);
                        break;
                    default:
                        break;
                }
                break;

            case OP(G_POPMTX):
                break;

            case OP(G_RDPHALF_1):
                g.rdpHalf1 = w1;
                break;

            case OP(G_RDPHALF_2):
                g.rdpHalf2 = w1;
                break;

            case OP(G_TEXRECT):
            case OP(G_TEXRECTFLIP): {
                // The texture coordinates follow in two G_RDPHALF commands.
                const Gfx *h1 = dl;
                const Gfx *h2 = dl + 1;
                g.rdpHalf1 = h1->words.w1;
                g.rdpHalf2 = h2->words.w1;
                dl += 2;
                remaining -= 2;
                texture_rectangle(w0, w1, op == G_TEXRECTFLIP);
                break;
            }

            case OP(G_FILLRECT):
                fill_rectangle(w0, w1);
                break;

            case OP(G_SETSCISSOR):
                g.scissor[0] = ((w0 >> 12) & 0xFFF) >> 2;
                g.scissor[1] = (w0 & 0xFFF) >> 2;
                g.scissor[2] = ((w1 >> 12) & 0xFFF) >> 2;
                g.scissor[3] = (w1 & 0xFFF) >> 2;
                break;

            case OP(G_SETCIMG):
                g.colorImage = seg_addr(w1);
                break;

            case OP(G_SETZIMG):
                g.depthImage = seg_addr(w1);
                break;

            case OP(G_SETTIMG):
                g.timg.fmt = (w0 >> 21) & 7;
                g.timg.siz = (w0 >> 19) & 3;
                g.timg.width = (w0 & 0xFFF) + 1;
                g.timg.addr = seg_addr(w1);
                break;

            case OP(G_SETCOMBINE):
                g.combineW0 = w0 & 0x00FFFFFF;
                g.combineW1 = w1;
                decode_combiner();
                break;

            case OP(G_SETENVCOLOR):
                unpack_rgba(w1, g.env);
                break;

            case OP(G_SETPRIMCOLOR):
                unpack_rgba(w1, g.prim);
                g.primLodFrac = w0 & 0xFF;
                break;

            case OP(G_SETBLENDCOLOR):
                unpack_rgba(w1, g.blendColor);
                break;

            case OP(G_SETFOGCOLOR):
                unpack_rgba(w1, g.fogColor);
                break;

            case OP(G_SETFILLCOLOR):
                g.fillColor = w1;
                break;

            case OP(G_SETTILE): {
                Tile *t = &g.tiles[(w1 >> 24) & 7];
                t->fmt = (w0 >> 21) & 7;
                t->siz = (w0 >> 19) & 3;
                t->line = (w0 >> 9) & 0x1FF;
                t->tmem = w0 & 0x1FF;
                t->palette = (w1 >> 20) & 0xF;
                t->cmt = (w1 >> 18) & 3;
                t->maskt = (w1 >> 14) & 0xF;
                t->shiftt = (w1 >> 10) & 0xF;
                t->cms = (w1 >> 8) & 3;
                t->masks = (w1 >> 4) & 0xF;
                t->shifts = w1 & 0xF;
                break;
            }

            case OP(G_SETTILESIZE): {
                Tile *t = &g.tiles[(w1 >> 24) & 7];
                t->uls = (w0 >> 12) & 0xFFF;
                t->ult = w0 & 0xFFF;
                t->lrs = (w1 >> 12) & 0xFFF;
                t->lrt = w1 & 0xFFF;
                break;
            }

            case OP(G_LOADBLOCK): {
                Tile *t = &g.tiles[(w1 >> 24) & 7];
                u32 texels = ((w1 >> 12) & 0xFFF) + 1;
                u32 dxt = w1 & 0xFFF;
                TmemLoad *load = &g.tmem[(t->tmem / 8) & 63];
                u32 bytes;
                switch (g.timg.siz) {
                    case G_IM_SIZ_4b: bytes = texels / 2; break;
                    case G_IM_SIZ_8b: bytes = texels; break;
                    case G_IM_SIZ_16b: bytes = texels * 2; break;
                    default: bytes = texels * 4; break;
                }
                load->addr = g.timg.addr + (((w0 >> 12) & 0xFFF) << g.timg.siz >> 1);
                load->size = bytes;
                load->rowStride = 0;
                load->lineSwapped = (dxt == 0);
                load->valid = TRUE;
                forget_covered_loads(t->tmem, bytes);
                break;
            }

            case OP(G_LOADTILE): {
                Tile *t = &g.tiles[(w1 >> 24) & 7];
                u32 uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2;
                u32 lrs = ((w1 >> 12) & 0xFFF) >> 2, lrt = (w1 & 0xFFF) >> 2;
                TmemLoad *load = &g.tmem[(t->tmem / 8) & 63];
                u32 bpp4 = 2 << g.timg.siz; // bits per texel / 2
                u32 stride = g.timg.width * bpp4 / 4;
                load->addr = g.timg.addr + ult * stride + uls * bpp4 / 4;
                load->rowStride = stride;
                load->size = (lrt - ult + 1) * stride;
                load->lineSwapped = FALSE;
                load->valid = TRUE;
                forget_covered_loads(t->tmem, load->size);
                (void) lrs;
                break;
            }

            case OP(G_LOADTLUT): {
                // TLUT entries sit from TMEM word 256, one per word: the tile's address says which entry this is.
                g.tlutAddr = g.timg.addr;
                g.tlutCount = ((w1 >> 14) & 0x3FF) + 1;
                g.tlutFirst = (s32) (g.tiles[(w1 >> 24) & 7].tmem & 0x1FF) - 256;
                break;
            }

            default:
                break;
        }
    }
}

static void run_dl(const Gfx *dl, s32 maxCommands) {
    run_command_list(dl, maxCommands, 0);
}

/******************************** Decoding textures at load time ********************************/

/*
 * Textures used to be decoded the first time a frame drew them, a hitch whenever the camera turned to something new.
 * Now the game hands each texture it loads to the renderer (port_gfx_texture_loaded, from load_texture), and the
 * renderer decodes it before its next frame, while the level is still loading. Textures the game frees
 * (port_gfx_texture_freed) leave the cache at the same point, so it only ever holds what the level uses and has no
 * reason to evict any of it. The game thread only writes the queue and the renderer only reads it.
 * DKR_TEX_PREWARM=0 goes back to decoding on first use.
 */
#define TEX_QUEUE_SIZE 1024
#define TEX_QUEUE_COMMANDS 20

typedef struct TexQueueEntry {
    const u8 *start, *end;      // A freed texture's memory
    Gfx cmd[TEX_QUEUE_COMMANDS]; // A loaded texture's load commands (a copy: the texture may be freed before it runs)
    s32 numCommands;            // 0 for a freed texture
} TexQueueEntry;

static TexQueueEntry sTexQueue[TEX_QUEUE_SIZE];
static u32 sTexQueueHead; // Written by the game threads, under sTexQueueLock
static u32 sTexQueueTail; // Written by the renderer
static u32 sTexQueueDropped;
static s32 sPrewarmOff = -1;
/* The game thread and the background track loader (thread30) both load and free textures, so writers take turns:
   otherwise both could fill the same slot, or the renderer read a slot still being filled. */
static PlatSem *sTexQueueLock;

/* Takes the writers' lock and returns the next free slot, or NULL (lock released) when the queue is full. */
static TexQueueEntry *tex_queue_slot(void) {
    u32 head;
    PlatSem *lock = __atomic_load_n(&sTexQueueLock, __ATOMIC_ACQUIRE);
    if (lock == NULL) {
        PlatSem *expected = NULL;
        lock = plat_sem_create(1);
        if (!__atomic_compare_exchange_n(&sTexQueueLock, &expected, lock, FALSE, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            lock = expected; // Another writer made it first (the one made here is never used: a one-off leak).
        }
    }
    plat_sem_wait(lock);
    head = sTexQueueHead;
    if (head - __atomic_load_n(&sTexQueueTail, __ATOMIC_ACQUIRE) >= TEX_QUEUE_SIZE) {
        sTexQueueDropped++; // The renderer catches up at its next frame; the texture is decoded when drawn instead.
        plat_sem_post(lock);
        return NULL;
    }
    return &sTexQueue[head % TEX_QUEUE_SIZE];
}

/* Publishes the slot from tex_queue_slot and releases the writers' lock. */
static void tex_queue_push(void) {
    __atomic_store_n(&sTexQueueHead, sTexQueueHead + 1, __ATOMIC_RELEASE);
    plat_sem_post(sTexQueueLock);
}

/* Game thread: a texture was loaded, and these commands (from material_init) load it for drawing. */
void port_gfx_texture_loaded(const void *commands, s32 numCommands) {
    const Gfx *cmd = (const Gfx *) commands;
    TexQueueEntry *e;
    s32 i;
    if (numCommands <= 0 || numCommands > TEX_QUEUE_COMMANDS || (e = tex_queue_slot()) == NULL) {
        return;
    }
    for (i = 0; i < numCommands; i++) {
        e->cmd[i] = cmd[i];
    }
    e->numCommands = numCommands;
    tex_queue_push();
}

/* Game thread: the texture data in [start, end) was freed. */
void port_gfx_texture_freed(const void *start, const void *end) {
    TexQueueEntry *e = tex_queue_slot();
    if (e == NULL) {
        return;
    }
    e->start = (const u8 *) start;
    e->end = (const u8 *) end;
    e->numCommands = 0;
    tex_queue_push();
}

/* Drops the cache entries (and remembered hashes) of texture data in [start, end). */
static void drop_textures(const u8 *start, const u8 *end) {
    s32 i;
    for (i = 0; i < TEX_CACHE_SIZE; i++) {
        if (sTexCacheAddr[i] >= start && sTexCacheAddr[i] < end) {
            free_entry(&sTexCache[i]);
        }
    }
    for (i = 0; i < HASH_CACHE_SIZE; i++) {
        if (sHashCache[i].data >= start && sHashCache[i].data < end) {
            sHashCache[i].data = NULL;
        }
    }
}

/*
 * Renderer, before a frame: decodes the textures loaded since the last one, and forgets the freed ones. The load
 * commands run against a scratch copy of the tile and TMEM state, with RGBA16 palettes. The filter doesn't matter: a
 * texture drawn with the other one shares the decoded copy (see texture_with_filter).
 */
static void prewarm_textures(void) {
    static Tile savedTiles[ARRAY_COUNT(g.tiles)];
    static TmemLoad savedTmem[ARRAY_COUNT(g.tmem)];
    static const TmemLoad noLoad;
    u32 head = __atomic_load_n(&sTexQueueHead, __ATOMIC_ACQUIRE);
    u32 tail = sTexQueueTail;
    u32 savedOmH;
    u64 savedTmemValid;
    const u8 *savedTlut;
    u32 savedTlutCount;
    s32 savedTlutFirst;
    s32 decoded = 0, freed = 0, i;
    unsigned long long start, elapsed;

    if (tail == head) {
        return;
    }
    if (sPrewarmOff < 0) {
        const char *env = plat_getenv("DKR_TEX_PREWARM");
        sPrewarmOff = (env != NULL && env[0] == '0');
    }
    if (sPrewarmOff) {
        __atomic_store_n(&sTexQueueTail, head, __ATOMIC_RELEASE);
        return;
    }
    start = plat_time_ns();
    for (i = 0; i < ARRAY_COUNT(g.tiles); i++) {
        savedTiles[i] = g.tiles[i];
    }
    for (i = 0; i < ARRAY_COUNT(g.tmem); i++) {
        savedTmem[i] = g.tmem[i];
    }
    savedTmemValid = g.tmemValid;
    savedOmH = g.omH;
    savedTlut = g.tlutAddr;
    savedTlutCount = g.tlutCount;
    savedTlutFirst = g.tlutFirst;
    g.omH = (g.omH & ~((3 << G_MDSFT_TEXTFILT) | (3 << G_MDSFT_TEXTLUT))) | G_TF_BILERP | G_TT_RGBA16;
    sPrewarming = TRUE;
    for (; tail != head; tail++) {
        TexQueueEntry *e = &sTexQueue[tail % TEX_QUEUE_SIZE];
        if (e->numCommands == 0) {
            drop_textures(e->start, e->end);
            freed++;
        } else {
            for (i = 0; i < ARRAY_COUNT(g.tmem); i++) {
                g.tmem[i] = noLoad;
            }
            g.tmemValid = 0;
            run_command_list(e->cmd, e->numCommands, 0);
            if (lookup_texture(G_TX_RENDERTILE, NULL) != NULL) {
                decoded++;
            }
        }
    }
    sPrewarming = FALSE;
    __atomic_store_n(&sTexQueueTail, tail, __ATOMIC_RELEASE);
    for (i = 0; i < ARRAY_COUNT(g.tiles); i++) {
        g.tiles[i] = savedTiles[i];
    }
    for (i = 0; i < ARRAY_COUNT(g.tmem); i++) {
        g.tmem[i] = savedTmem[i];
    }
    g.tmemValid = savedTmemValid;
    g.omH = savedOmH;
    g.tlutAddr = savedTlut;
    g.tlutCount = savedTlutCount;
    g.tlutFirst = savedTlutFirst;
    sTriStateValid = FALSE;
    elapsed = plat_time_ns() - start;
    // Level loads, not the odd object texture coming and going mid-race.
    if (decoded + freed >= 16 || elapsed >= 1000000) {
        plat_log("textures: %d decoded at load, %d freed, %.1f ms (frame %d, %u replaced while in use, %u not queued)",
                 (int) decoded, (int) freed, elapsed / 1e6, (int) sFrameCount, (unsigned) sRecentEvictions,
                 (unsigned) sTexQueueDropped);
    }
}

/******************************** Entry points ********************************/

static s32 sGfxInitialised;

void port_gfx_init(void) {
    if (!sGfxInitialised) {
        const char *trace = plat_getenv("DKR_TRACE_FRAME");
        sGfxInitialised = TRUE;
        sTraceFrame = (trace != NULL) ? port_atoi(trace) : -1;
        gfxb_init();
#ifdef PORT_GFX_REFERENCE
        {
            const char *diff = plat_getenv("DKR_DIFF_EVERY");
            sDiffEvery = (diff != NULL) ? port_atoi(diff) : 0;
            if (sDiffEvery > 0) {
                refb_init();
                sRefInitialised = TRUE;
            }
        }
#endif
    }
}

#ifdef PORT_GFX_REFERENCE
/* A snapshot: the frame is also drawn by the reference renderer, both images are saved as for DKR_DIFF_EVERY, and
 * every draw is logged. */
static COLD void start_snapshot(void) {
    char path[256];
    gPlatSnapshotRequested = 0;
    if (!sRefInitialised) {
        refb_init();
        sRefInitialised = TRUE;
    }
    sRefActive = TRUE;
    sTraceFrame = sFrameCount;
    plat_log_keep();
    plat_snprintf(path, sizeof(path), "%s/diff_%05d_log.txt", plat_data_dir(), (int) sFrameCount);
    plat_log_copy_to(path);
    gfxb_log_draws((int) sFrameCount);
    plat_log("snapshot of frame %d", (int) sFrameCount);
}

/* Saves the reference renderer's and the GPU's pictures of a snapshot or DKR_DIFF_EVERY frame. */
static COLD void end_reference_frame(void) {
    char path[256];
    refb_end_frame();
    plat_snprintf(path, sizeof(path), "%s/diff_%05d_ref.png", plat_data_dir(), (int) sFrameCount);
    refb_save_png(path);
    plat_snprintf(path, sizeof(path), "%s/diff_%05d_gpu.bin", plat_data_dir(), (int) sFrameCount);
    gfxb_capture_frame(path);
    sRefActive = FALSE;
    // The renderer closes the snapshot's log once the GPU has drawn the frame.
}
#endif

void port_gfx_run_task(void *taskPtr) {
    OSTask *task = (OSTask *) taskPtr;
    int snapshot;
    port_gfx_init();
    sFrameCount++;
    sDrawKind = PORT_DRAW_OTHER;
#ifdef PORT_GFX_REFERENCE
    sRefActive = (sDiffEvery > 0 && sFrameCount % sDiffEvery == 0);
#endif
    // A frame the walk needs drawn at once (a snapshot, a traced frame, the reference renderer's) is made directly;
    // so are frames with GPU vertex maths, whose walk asks the backend about each model (gfxb_model_check).
    snapshot = gPlatSnapshotRequested; // Read once: another thread sets it
    gfx_split_frame_start(REF_ACTIVE() || snapshot || sFrameCount == sTraceFrame ||
                          plat_switch("DKR_GPU_TRANSFORM", plat_gpu_maths_setting() >= 1));
#ifdef PORT_GFX_REFERENCE
    if (UNLIKELY(snapshot)) {
        start_snapshot();
    }
#endif
    REF_CALL(refb_start_frame());
    sTracing = sFrameCount == sTraceFrame;
    read_switches();
    prewarm_textures();
    forget_all_loads();
    sGpuTransform = !gfx_split_active() && gfxb_gpu_transform();
    sCpuCull = sCpuCullSwitch && gfxb_cpu_cull_ok() && !gGfxSweep;
    sTrackMesh = sGpuTransform && sTrackMeshSwitch;
    sModelMesh = sGpuTransform && sModelMeshSwitch;
    if (!sTrackMesh && !sModelMesh) {
        if (sMeshBytes > 0) {
            gfx_split_drain(); // Frees go through the backend's frame slot, which the second renderer thread may be using
            mesh_drop_all();
        }
    } else {
        mesh_age();
        if (sFrameCount % 600 == 0) {
            plat_log("meshes: %u KB, %u draws, %u made, %u not recorded, %u data changes", (unsigned) (sMeshBytes / 1024),
                     (unsigned) sMeshStats[0], (unsigned) sMeshStats[1], (unsigned) sMeshStats[2],
                     (unsigned) sMeshStats[3]);
        }
    }
    g.vertexIndex = 0;
    g.billboard = 0;
    sTriStateValid = FALSE;
    if (g.scissor[2] == 0) {
        g.scissor[2] = GFX_SCREEN_WIDTH;
        g.scissor[3] = GFX_SCREEN_HEIGHT;
    }
    gPlatGfxStage = "running a display list";
    walk_begin();
    run_dl((const Gfx *) task->t.data_ptr, -1);
    flush_batch();
    gPlatGfxStage = "ending the frame";
    walk_switch(WALK_FRAMEEND);
    gfx_split_frame_end();
    walk_end();
#ifdef PORT_GFX_REFERENCE
    if (UNLIKELY(sRefActive)) {
        end_reference_frame();
    }
#endif
}

/* The number of frames drawn so far, for logs that need to line up with frame dumps. */
s32 port_gfx_frame_count(void) {
    return sFrameCount;
}
