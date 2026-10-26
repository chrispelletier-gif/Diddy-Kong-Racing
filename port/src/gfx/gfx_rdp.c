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

#define MAX_VERTICES 64
#define MAX_BATCH_TRIANGLES GFX_MAX_BATCH_TRIANGLES
#define MAX_BATCH_VERTICES GFX_MAX_BATCH_VERTICES
#define DL_STACK_SIZE 18

/******************************** State ********************************/

typedef struct RspVertex {
    float x, y, z, w; // Clip space, already including the viewport transform (see gfx_backend.h)
    u8 r, g, b, a;
    u8 fog;
    u8 fogValid; // The fog factor is only worked out while G_FOG is set (see load_vertices)
    u8 flat;     // Loaded with an orthographic matrix: drawn flat on the screen (1, or GFX_FLAT_PLACED for a sprite)
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
    const u8 *tlutAddr;
    u32 tlutCount;
    const u8 *colorImage;
    const u8 *depthImage;
    s16 scissor[4];
    u32 rdpHalf1, rdpHalf2;
} g;

static s32 sTraceFrame = -1;

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
        if (sRefActive) {                                                                                              \
            call;                                                                                                      \
        }                                                                                                              \
    } while (0)
#else
#define REF_CALL(call)
#endif
#define TRACE(...)                        \
    if (sFrameCount == sTraceFrame) {     \
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
/* Changes whenever the batch's vertices become invalid (a flush, or a new triangle command): entries of the
 * triangle command's shared vertex table are only valid for the generation they were made in. */
static u32 sBatchGeneration;

/*
 * The draw state of the last triangle command. Between two triangle commands DKR usually only loads vertices or
 * matrices, so the next one can reuse it instead of building it (and looking up its textures) again. Any command that
 * can change the draw state clears sTriStateValid (see state_command).
 */
static GfxDrawState sTriState;
static s32 sTriStateValid;
static s32 sFrameCount;

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
static u32 sample_hash(const u8 *data, u32 size) {
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

static void decode_texture(u32 *out, const TmemLoad *load, const Tile *tile, u32 width, u32 height, const u8 *tlut) {
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
                        const u8 *p = tlut + ((tile->palette << 4) + b0) * 2;
                        c = (g.omH & (3 << G_MDSFT_TEXTLUT)) == G_TT_IA16 ? ia16_to_8888((p[0] << 8) | p[1])
                                                                          : rgba5551_to_8888((p[0] << 8) | p[1]);
                    } else if (tile->fmt == G_IM_FMT_IA) {
                        u32 i = (b0 >> 1) * 255 / 7;
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
                        c = rgba5551_to_8888((p[0] << 8) | p[1]);
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
                return GFX_TEXFMT_RGBA5551;
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
    return sBatchCount > 0 && (&e->tex == sBatchState.textures[0] || &e->tex == sBatchState.textures[1] ||
                               other == sBatchState.textures[0] || other == sBatchState.textures[1]);
}

/* Frees a cache entry's textures. */
static void free_entry(TexCacheEntry *e) {
    if (e->hasOtherFilter) {
        gfxb_texture_free(&e->otherFilter);
        e->hasOtherFilter = FALSE;
    }
    gfxb_texture_free(&e->tex);
    plat_free((void *) e->tex.pixels);
    e->used = FALSE;
    sTexCacheAddr[e - sTexCache] = NULL;
}

/*
 * The entry's texture for drawing with the given filter. Mostly DKR draws a texture with one filter, but HUD and
 * menu textures are drawn both ways, and a texture decoded at load time doesn't know yet which it will be. The other
 * filter is a second texture object sharing the decoded pixels and GPU copy, so it costs no decode or upload; it
 * can't be the same object, since changing the filter in place would also change it for triangles already batched.
 */
static GfxTexture *texture_with_filter(TexCacheEntry *e, u8 filter) {
    if (e->tex.filter == filter) {
        return &e->tex;
    }
    if (!e->hasOtherFilter) {
        e->otherFilter = e->tex;
        e->otherFilter.filter = filter;
        e->otherFilter.backendData = NULL;
        gfxb_texture_share(&e->otherFilter, &e->tex);
        e->hasOtherFilter = TRUE;
    }
    return &e->otherFilter;
}

/*
 * sample_hash() for this frame. The same texture is often loaded many times in a frame, and RDRAM doesn't change
 * while a display list runs, so its hash is only worked out the first time.
 */
typedef struct HashCacheEntry {
    const u8 *data;
    u32 size, frame, hash;
} HashCacheEntry;

#define HASH_CACHE_SIZE 256

static HashCacheEntry sHashCache[HASH_CACHE_SIZE];

static u32 frame_hash(const u8 *data, u32 size) {
    HashCacheEntry *h = &sHashCache[(((uintptr_t) data >> 3) ^ size) % HASH_CACHE_SIZE];
    if (h->frame != (u32) sFrameCount || h->data != data || h->size != size) {
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

/* Returns the texture for the given tile, decoding it if it isn't in the cache. */
static GfxTexture *lookup_texture(s32 tileIndex, const GfxTexture *pinned) {
    Tile *tile = &g.tiles[tileIndex & 7];
    TmemLoad *load = &g.tmem[(tile->tmem / 8) & 63];
    u32 width, height, size, hash, i, slot, oldest;
    const u8 *tlut = NULL;
    u8 filter, wrapS, wrapT, tlutType;
    TexCacheEntry *e;
    u32 *pixels;
    unsigned long long texStart;

    if (!load->valid) {
        // Loaded at an unaligned TMEM address. Search for the load covering it.
        for (i = 0; i < 64; i++) {
            if (g.tmem[i].valid && i * 8 <= tile->tmem && tile->tmem < i * 8 + (g.tmem[i].size + 7) / 8) {
                load = &g.tmem[i];
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
    if (load->rowStride == 0 && tile->line != 0) {
        u32 rows = load->size / tile_row_bytes(tile);
        if (rows == 0) {
            rows = 1;
        }
        if (height > rows) {
            height = rows;
        }
    }
    if (tile->fmt == G_IM_FMT_CI) {
        tlut = g.tlutAddr;
        if (tlut == NULL) {
            return NULL;
        }
    }
    size = load->rowStride ? load->rowStride * height : load->size;
    hash = frame_hash(load->addr, size);
    if (tlut != NULL) {
        hash ^= frame_hash(tlut, (tile->siz == G_IM_SIZ_4b) ? 32 : 512) * 31;
    }
    // Copy mode never filters.
    filter = ((g.omH & (3 << G_MDSFT_TEXTFILT)) == G_TF_POINT || cycle_type() == G_CYC_COPY) ? GFX_FILTER_POINT
                                                                                            : GFX_FILTER_BILINEAR;
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

    texStart = plat_time_ns();
    e = &sTexCache[oldest];
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
    e->tex.filter = filter;
    e->tex.wrapS = wrapS;
    e->tex.wrapT = wrapT;
    e->tex.backendData = NULL;
    gfxb_texture_upload(&e->tex);
    prof_add(PROF_TEXTURE, plat_time_ns() - texStart);
    if (!sPrewarming) {
        // Only the decodes that interrupt a frame: these are the hitches.
        prof_count(PROF_TEXTURE_UPLOADS, 1);
    }
    return &e->tex;
}

/*
 * Converts texture coordinates in 10.5 texels to normalised coordinates of the tile's texture. The per-draw part is
 * worked out once, so there are no divisions per vertex (slow on the 3DS's VFP), and the results are the same as
 * dividing each time: the shift and the 1/32 are powers of two, so one combined factor is exact, and so is the
 * reciprocal of a power of two texture size (other sizes still divide).
 */
typedef struct TexAxis {
    float scale, offset, size, invSize;
    s32 divide;
} TexAxis;

/* The RDP's bilinear filter puts texel centres on whole texture coordinates, while GPUs (and the software renderer)
 * put them half a texel further on, so filtered textures are moved by that half texel to land where the N64 draws them.
 * Point sampled ones line up already. */
static inline float texel_centre_shift(const GfxTexture *tex) {
    return (tex->filter == GFX_FILTER_BILINEAR) ? 0.5f : 0.0f;
}

static void tex_axis_init(TexAxis *a, u8 shift, u16 tileStart, u32 size, float centreShift) {
    float scale = 1.0f / 32.0f;
    if (shift > 10) {
        scale *= (float) (1 << (16 - shift));
    } else if (shift > 0) {
        scale /= (float) (1 << shift);
    }
    a->scale = scale;
    a->offset = tileStart * 0.25f - centreShift;
    a->size = (float) size;
    a->divide = (size & (size - 1)) != 0;
    a->invSize = 1.0f / (float) size;
}

static inline float tex_axis_coord(const TexAxis *a, s32 coord) {
    float c = coord * a->scale - a->offset;
    return a->divide ? c / a->size : c * a->invSize;
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

static void decode_combiner(void) {
    u32 w0 = g.combineW0, w1 = g.combineW1;
    GfxCombiner *cc = &g.combiner;
    s32 i;

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
    for (i = 0; i < 2; i++) {
        const GfxCCEquation *eqs[2] = { &cc->rgb[i], &cc->alpha[i] };
        u8 uses = 0;
        s32 j;
        for (j = 0; j < 2; j++) {
            u8 in[4] = { eqs[j]->a, eqs[j]->b, eqs[j]->c, eqs[j]->d };
            s32 k;
            for (k = 0; k < 4; k++) {
                if (in[k] == GFX_CC_TEXEL0 || in[k] == GFX_CC_TEXEL0_ALPHA) {
                    uses |= 1;
                }
                if (in[k] == GFX_CC_TEXEL1 || in[k] == GFX_CC_TEXEL1_ALPHA) {
                    uses |= 2;
                }
            }
        }
        g.combinerTextures[i + 1] = g.combinerTextures[i] | uses;
    }
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

/* Builds the draw state for the current RSP/RDP state. */
static void build_draw_state(GfxDrawState *s, s32 isRect) {
    u32 cyc = cycle_type();
    u32 omL = g.omL;
    u32 blendBits;
    u32 p, a, m, b;
    s32 usesTex0 = FALSE, usesTex1 = FALSE;
    s32 i;

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
    for (i = 0; i < 4; i++) {
        s->prim[i] = g.prim[i];
        s->env[i] = g.env[i];
        s->fog[i] = g.fogColor[i];
    }
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
    for (i = 0; i < 4; i++) {
        s->scissor[i] = g.scissor[i];
    }
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

static s32 draw_states_equal(const GfxDrawState *a, const GfxDrawState *b) {
    return __builtin_memcmp(a, b, sizeof(GfxDrawState)) == 0;
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

static void flush_batch(void) {
    if (sBatchCount > 0) {
        if (sFrameCount == sTraceFrame) {
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
            plat_log("    %d vertices, v0 %.3f %.3f %.3f %.3f uv %.3f %.3f rgba %d %d %d %d", (int) sBatchVertices, v->x,
                     v->y, v->z, v->w, v->u0, v->v0, v->r, v->g, v->b, v->a);
            plat_log("    u %.3f..%.3f v %.3f..%.3f w %.1f..%.1f", minU, maxU, minV, maxV, minW, maxW);
        }
        gfxb_draw_indexed(&sBatchState, sBatchVerts, sBatchVertices, sBatchIndices, sBatchCount);
        prof_count(PROF_TRIANGLES, sBatchCount);
        prof_count(PROF_DRAWS, 1);
        REF_CALL(refb_draw_indexed(&sBatchState, sBatchVerts, sBatchVertices, sBatchIndices, sBatchCount));
        sBatchCount = 0;
        sBatchVertices = 0;
        sBatchGeneration++;
    }
}

/* Whether the batch has room for one more triangle with up to three new vertices. */
static inline s32 batch_has_room(void) {
    return sBatchCount < MAX_BATCH_TRIANGLES && sBatchVertices + 3 <= MAX_BATCH_VERTICES;
}

/* Makes the batch one of this state with room for the triangles and vertices, flushing it first if it is full or of
 * another state. */
static void batch_reserve(const GfxDrawState *state, s32 triangles, s32 vertices) {
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

static void load_matrix(const Mtx *mtx, s32 index) {
    const s32 *m = (const s32 *) mtx;
    s32 i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j += 2) {
            s32 intPart = m[i * 2 + j / 2];
            s32 fracPart = m[8 + i * 2 + j / 2];
            s32 a = (intPart & 0xFFFF0000) | ((fracPart >> 16) & 0xFFFF);
            s32 b = ((intPart << 16) & 0xFFFF0000) | (fracPart & 0xFFFF);
            g.matrices[index][i][j] = a * (1.0f / 65536.0f);
            g.matrices[index][i][j + 1] = b * (1.0f / 65536.0f);
        }
    }
}

/* Fog, as the RSP computes it from the normalised depth. */
static void vertex_fog(RspVertex *v) {
    float w = (v->w != 0.0f) ? v->w : 0.0001f;
    // Clamp before converting: out of range float to int conversions differ between CPUs.
    float fog = (v->z / w) * g.fogMul + g.fogOffset;
    v->fog = (fog < 0.0f) ? 0 : (fog > 255.0f) ? 255 : (u8) fog;
}

static void load_vertices(const Vertex *src, s32 count, s32 dest) {
    float(*m)[4] = g.matrices[g.curMatrix];
    s32 i, fog = (g.geometryMode & G_FOG) != 0;
    // An orthographic matrix (the HUD's, menus', fades') gives every vertex the same w.
    u8 flat = m[0][3] == 0.0f && m[1][3] == 0.0f && m[2][3] == 0.0f;
    // Billboard (sprite) vertices are offsets in clip space, sized for the N64's camera; on the port's wider (G3) or
    // narrower (the widescreen style's field of view) camera they are scaled to keep their size next to the 3D world.
    // Their matrix has no perspective, but they belong with their anchor: the 3D world's, or, anchored by the HUD's
    // or a menu's orthographic matrix (the Save Options balloons, the banana counter), the flat 4:3 picture, where
    // they never stretch to a wide screen's edge as they pass the N64 one (GFX_FLAT_PLACED).
    // Their depth offsets too are sized for the N64's camera, whose near plane is at 10: widescreen's nearer one (2.5,
    // CAMERA_NEAR in src/camera.h) maps the same clip-space offset to 4 times the depth, which stood Player Select's
    // bushes in front of the log they grow behind. So in widescreen the offset past the anchor's depth (the depth
    // offset less the matrix's z scale, about 1, that comes with the vertex's extra w of 1) is scaled by 2.5 / 10.
    float bbScaleY = 1.0f, bbScaleX = 1.0f, bbScaleZ = 1.0f;
    u8 bbFlat = g.verts[0].flat ? GFX_FLAT_PLACED : 0;
    if (g.billboard && !bbFlat) {
        bbScaleY = 1.0f / plat_fov_tan_scale();
        bbScaleX = bbScaleY / plat_aspect_scale();
        bbScaleZ = plat_aspect_scale() > 1.0f ? 2.5f / 10.0f : 1.0f;
    }

    for (i = 0; i < count && dest + i < MAX_VERTICES; i++) {
        RspVertex *v = &g.verts[dest + i];
        float x = src[i].x, y = src[i].y, z = src[i].z;
        v->x = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
        v->y = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
        v->z = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
        v->w = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
        v->flat = flat;
        if (g.billboard && dest + i > 0) {
            // Billboard vertices are offsets from the anchor vertex, in clip space.
            v->x = v->x * bbScaleX + g.verts[0].x;
            v->y = v->y * bbScaleY + g.verts[0].y;
            v->z = (v->z - 1.0f) * bbScaleZ + 1.0f + g.verts[0].z;
            v->w += g.verts[0].w;
            v->flat = bbFlat;
        }
        v->r = src[i].r;
        v->g = src[i].g;
        v->b = src[i].b;
        v->a = src[i].a;
        // The fog factor costs a divide, so it is only worked out for vertices loaded with fog on (all fogged
        // geometry, in practice); draw_dkr_triangles catches any others if fog is turned on before they are drawn.
        v->fogValid = fog;
        if (fog) {
            vertex_fog(v);
        } else {
            g.vertsWithoutFog = TRUE;
        }
    }
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

/* With G_FOG set the RSP replaces the shade alpha with the fog factor. */
static void finish_vertex(GfxVertex *out, const RspVertex *in, const Viewport *vp, s32 fog) {
    out->x = in->x * vp->scaleX + in->w * vp->transX;
    out->y = in->y * vp->scaleY + in->w * vp->transY;
    out->z = in->z;
    out->w = in->w;
    out->r = in->r;
    out->g = in->g;
    out->b = in->b;
    out->a = fog ? in->fog : in->a;
}

/*
 * The batch vertices made so far for each of the RSP's vertices in the triangle command being drawn: a DKR triangle
 * carries its own texture coordinates for each corner, so a vertex is shared only between corners with the same
 * ones. In the game's models about half the corners are such repeats, nearly always among the last few uses.
 */
#define SHARED_PER_VERTEX 4
typedef struct SharedVertex {
    u32 generation; // sBatchGeneration the entries were made in
    u32 key[SHARED_PER_VERTEX]; // Texture coordinates (u << 16 | v)
    u16 index[SHARED_PER_VERTEX];
    u8 count;
} SharedVertex;
static SharedVertex sShared[MAX_VERTICES];

/* Whether a flat triangle reaches both sides of the N64 screen (a full screen picture), as gfx_flat_x sees them. */
static s32 flat_triangle_spans_screen(const u8 idx[3], const Viewport *vp) {
    s32 k, left = FALSE, right = FALSE;
    for (k = 0; k < 3; k++) {
        const RspVertex *v = &g.verts[idx[k]];
        float x = v->x * vp->scaleX + v->w * vp->transX, edge = 0.99f * (v->w < 0.0f ? -v->w : v->w);
        left |= x <= -edge;
        right |= x >= edge;
    }
    return left && right;
}

static void draw_dkr_triangles(const Triangle *tris, s32 count, s32 textured) {
    GfxDrawState state;
    s32 i, k, fog = (g.geometryMode & G_FOG) != 0;
    Tile *tile0 = &g.tiles[0], *tile1 = &g.tiles[1];
    float w0 = 1.0f, h0 = 1.0f, w1 = 1.0f, h1 = 1.0f;
    TexAxis u0, v0, u1, v1;
    Viewport vp;
    u8 batchedKey = 0xFF; // The batch's culling and flatness
    u8 frontCull = g.vpScale[0] < 0.0f ? 2 : 1;
    s32 tex0, tex1;

    if (!sTriStateValid) {
        build_draw_state(&sTriState, FALSE);
        sTriStateValid = TRUE;
    }
    state = sTriState;
    if (fog && g.vertsWithoutFog) {
        for (i = 0; i < MAX_VERTICES; i++) {
            if (!g.verts[i].fogValid) {
                vertex_fog(&g.verts[i]);
                g.verts[i].fogValid = TRUE;
            }
        }
        g.vertsWithoutFog = FALSE;
    }
    if (!textured) {
        state.textures[0] = NULL;
        state.textures[1] = NULL;
    }
    if (state.textures[0] != NULL) {
        w0 = state.textures[0]->width;
        h0 = state.textures[0]->height;
    }
    if (state.textures[1] != NULL) {
        w1 = state.textures[1]->width;
        h1 = state.textures[1]->height;
    }
    viewport_init(&vp);
    // Texture coordinates are only worked out for the textures the draw uses; the others are left at zero.
    tex0 = state.textures[0] != NULL;
    tex1 = state.textures[1] != NULL;
    if (tex0) {
        float shift = texel_centre_shift(state.textures[0]);
        tex_axis_init(&u0, tile0->shifts, tile0->uls, (u32) w0, shift);
        tex_axis_init(&v0, tile0->shiftt, tile0->ult, (u32) h0, shift);
    }
    if (tex1) {
        float shift = texel_centre_shift(state.textures[1]);
        tex_axis_init(&u1, tile1->shifts, tile1->uls, (u32) w1, shift);
        tex_axis_init(&v1, tile1->shiftt, tile1->ult, (u32) h1, shift);
    }
    // Vertices of earlier commands (or loads) can't be shared: the vertex data, viewport and fog may have changed.
    sBatchGeneration++;
    for (i = 0; i < count; i++) {
        const Triangle *t = &tris[i];
        const TexCoords *uv[3] = { &t->uv0, &t->uv1, &t->uv2 };
        u8 idx[3] = { t->vi0, t->vi1, t->vi2 };
        u8 flat0, flat1, flat2;
        u16 *out;

        if (idx[0] >= MAX_VERTICES || idx[1] >= MAX_VERTICES || idx[2] >= MAX_VERTICES) {
            continue;
        }
        // Backface culling: flag 0x40 means double sided. The game mirrors the viewport for mirror mode.
        state.cullMode = (t->flags & BACKFACE_DRAW) ? 0 : frontCull;
        flat0 = g.verts[idx[0]].flat;
        flat1 = g.verts[idx[1]].flat;
        flat2 = g.verts[idx[2]].flat;
        state.flat = (flat0 && flat1 && flat2) ? (flat0 | flat1 | flat2) : 0; // 1, or GFX_FLAT_PLACED (3) if a sprite
        if (state.flat == 1 && tex0 && !flat_triangle_spans_screen(idx, &vp)) {
            // Textured and narrower than the screen (a menu's wooden box): on a wide screen it keeps its shape as it
            // slides past the N64 edge, rather than a corner near the edge snapping out to the wide one's.
            state.flat = GFX_FLAT_PLACED;
        }
        if ((state.cullMode | (state.flat << 2)) == batchedKey && sBatchCount > 0 && batch_has_room()) {
            // Only the culling and flatness change from one triangle to the next, so the batch's state is still this
            // one.
            out = &sBatchIndices[sBatchCount++ * 3];
        } else {
            out = batch_triangle(&state);
            batchedKey = state.cullMode | (state.flat << 2);
        }
        for (k = 0; k < 3; k++) {
            SharedVertex *shared = &sShared[idx[k]];
            u32 key = ((u32) (u16) uv[k]->u << 16) | (u16) uv[k]->v;
            GfxVertex *v;
            s32 q, n;

            if (shared->generation != sBatchGeneration) {
                shared->generation = sBatchGeneration;
                shared->count = 0;
            }
            n = shared->count;
            for (q = 0; q < n; q++) {
                if (shared->key[q] == key) {
                    break;
                }
            }
            if (q < n) {
                out[k] = shared->index[q];
                continue;
            }
            out[k] = batch_vertex(&v);
            if (n < SHARED_PER_VERTEX) {
                shared->key[n] = key;
                shared->index[n] = out[k];
                shared->count = n + 1;
            }
            finish_vertex(v, &g.verts[idx[k]], &vp, fog);
            if (tex0) {
                v->u0 = tex_axis_coord(&u0, uv[k]->u);
                v->v0 = tex_axis_coord(&v0, uv[k]->v);
            } else {
                v->u0 = v->v0 = 0.0f;
            }
            if (tex1) {
                v->u1 = tex_axis_coord(&u1, uv[k]->u);
                v->v1 = tex_axis_coord(&v1, uv[k]->v);
            } else {
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

static void texture_rectangle(u32 w0, u32 w1, s32 flip) {
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
        // Copy mode draws 4 pixels per step and includes the last column/row.
        dsdx *= 0.25f;
        lrx += 1.0f;
        lry += 1.0f;
    }
    if (flip) {
        s1 = s + dtdy * (lry - uly);
        t1 = t + dsdx * (lrx - ulx);
    } else {
        s1 = s + dsdx * (lrx - ulx);
        t1 = t + dtdy * (lry - uly);
    }
    // draw_rect looks the tiles up through rdpHalf1, so stash the tile number there.
    g.rdpHalf1 = tile;
    draw_rect(ulx, uly, lrx, lry, s, t, s1, t1, flip, TRUE);
}

static void fill_rectangle(u32 w0, u32 w1) {
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
        flush_batch();
        if (g.colorImage == g.depthImage) {
            gfxb_clear_depth(rect);
            REF_CALL(refb_clear_depth(rect));
        } else {
            u32 c = rgba5551_to_8888(g.fillColor >> 16);
            gfxb_clear_color(rect, c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 255);
            REF_CALL(refb_clear_color(rect, c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 255));
        }
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

/* Whether a command can change what build_draw_state builds. Viewport and matrix changes don't (culling is set per
 * triangle). */
static inline s32 state_command(u32 op) {
    switch (op) {
        case OP(G_VTX):
        case OP(G_TRIN):
        case OP(G_MTX):
        case OP(G_POPMTX):
        case OP(G_DL):
        case OP(G_DMADL):
        case OP(G_ENDDL):
        case OP(G_MOVEMEM):
        case OP(G_SPNOOP):
        case OP(G_NOOP):
        case OP(G_RDPLOADSYNC):
        case OP(G_RDPPIPESYNC):
        case OP(G_RDPTILESYNC):
        case OP(G_RDPFULLSYNC):
            return FALSE;
        case OP(G_MOVEWORD):
            return FALSE; // Checked below: only G_MW_FOG matters.
        default:
            return TRUE;
    }
}

static void run_command_list(const Gfx *dl, s32 maxCommands, s32 depth) {
    s32 executed = 0;

    while (maxCommands < 0 || executed < maxCommands) {
        u32 w0 = dl->words.w0;
        u32 w1 = dl->words.w1;
        u32 op = w0 >> 24;
        dl++;
        executed++;
        TRACE("%*s%08X %08X", (int) (depth * 2), "", (unsigned) w0, (unsigned) w1);
        if (state_command(op)) {
            sTriStateValid = FALSE;
        }

        switch (op) {
            case OP(G_SPNOOP):
            case OP(G_NOOP):
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
                break;
            }

            case OP(G_TRIN):
                draw_dkr_triangles((const Triangle *) seg_addr(w1), (w0 >> 4) & 0xFFF, (w0 >> 16) & 1);
                g.vertexIndex = 0;
                break;

            case OP(G_DL):
                if (depth < DL_STACK_SIZE) {
                    run_command_list((const Gfx *) seg_addr(w1), -1, depth + 1);
                }
                if ((w0 >> 16) & G_DL_NOPUSH) {
                    return;
                }
                break;

            case OP(G_DMADL):
                if (depth < DL_STACK_SIZE) {
                    run_command_list((const Gfx *) seg_addr(w1), (w0 >> 16) & 0xFF, depth + 1);
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
                executed += 2;
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
                (void) lrs;
                break;
            }

            case OP(G_LOADTLUT): {
                g.tlutAddr = g.timg.addr;
                g.tlutCount = ((w1 >> 14) & 0x3FF) + 1;
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
static u32 sTexQueueHead; // Written by the game thread
static u32 sTexQueueTail; // Written by the renderer
static u32 sTexQueueDropped;
static s32 sPrewarmOff = -1;

static TexQueueEntry *tex_queue_slot(void) {
    u32 head = sTexQueueHead;
    if (head - __atomic_load_n(&sTexQueueTail, __ATOMIC_ACQUIRE) >= TEX_QUEUE_SIZE) {
        sTexQueueDropped++; // The renderer catches up at its next frame; the texture is decoded when drawn instead.
        return NULL;
    }
    return &sTexQueue[head % TEX_QUEUE_SIZE];
}

static void tex_queue_push(void) {
    __atomic_store_n(&sTexQueueHead, sTexQueueHead + 1, __ATOMIC_RELEASE);
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
    const u8 *savedTlut;
    u32 savedTlutCount;
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
    savedOmH = g.omH;
    savedTlut = g.tlutAddr;
    savedTlutCount = g.tlutCount;
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
    g.omH = savedOmH;
    g.tlutAddr = savedTlut;
    g.tlutCount = savedTlutCount;
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

void port_gfx_run_task(void *taskPtr) {
    OSTask *task = (OSTask *) taskPtr;
    port_gfx_init();
    sFrameCount++;
#ifdef PORT_GFX_REFERENCE
    sRefActive = (sDiffEvery > 0 && sFrameCount % sDiffEvery == 0);
#endif
    gfxb_start_frame();
#ifdef PORT_GFX_REFERENCE
    if (gPlatSnapshotRequested) {
        // A snapshot: the frame is also drawn by the reference renderer, both images are saved as for DKR_DIFF_EVERY,
        // and every draw is logged.
        gPlatSnapshotRequested = 0;
        if (!sRefInitialised) {
            refb_init();
            sRefInitialised = TRUE;
        }
        sRefActive = TRUE;
        sTraceFrame = sFrameCount;
        plat_log_keep();
        {
            char path[256];
            plat_snprintf(path, sizeof(path), "%s/diff_%05d_log.txt", plat_data_dir(), (int) sFrameCount);
            plat_log_copy_to(path);
        }
        gfxb_log_draws((int) sFrameCount);
        plat_log("snapshot of frame %d", (int) sFrameCount);
    }
#endif
    REF_CALL(refb_start_frame());
    prewarm_textures();
    g.vertexIndex = 0;
    g.billboard = 0;
    sTriStateValid = FALSE;
    if (g.scissor[2] == 0) {
        g.scissor[2] = GFX_SCREEN_WIDTH;
        g.scissor[3] = GFX_SCREEN_HEIGHT;
    }
    gPlatGfxStage = "running a display list";
    run_dl((const Gfx *) task->t.data_ptr, -1);
    flush_batch();
    gPlatGfxStage = "ending the frame";
    gfxb_end_frame();
#ifdef PORT_GFX_REFERENCE
    if (sRefActive) {
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
}

/* The number of frames drawn so far, for logs that need to line up with frame dumps. */
s32 port_gfx_frame_count(void) {
    return sFrameCount;
}
