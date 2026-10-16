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
#include "gfx_backend.h"

#define MAX_VERTICES 64
#define MAX_BATCH_TRIANGLES 256
#define DL_STACK_SIZE 18

/******************************** State ********************************/

typedef struct RspVertex {
    float x, y, z, w; // Clip space, already including the viewport transform (see gfx_backend.h)
    u8 r, g, b, a;
    u8 fog;
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
    u32 geometryMode;
    float vpScale[3], vpTrans[3];
    s16 fogMul, fogOffset;

    // RDP
    u32 omH, omL;
    u32 combineW0, combineW1;
    GfxCombiner combiner;
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
#define TRACE(...)                        \
    if (sFrameCount == sTraceFrame) {     \
        plat_log(__VA_ARGS__);            \
    }

static GfxVertex sBatchVerts[MAX_BATCH_TRIANGLES * 3];
static s32 sBatchCount;
static GfxDrawState sBatchState;
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

#define TEX_CACHE_SIZE 1024

typedef struct TexCacheEntry {
    const u8 *addr;
    const u8 *tlut;
    u32 hash;
    u16 width, height;
    u8 fmt, siz, lineSwapped, used;
    u8 filter, wrapS, wrapT;
    u32 lastFrame;
    GfxTexture tex;
} TexCacheEntry;

static TexCacheEntry sTexCache[TEX_CACHE_SIZE];

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

static u8 wrap_mode(u8 cm, u8 mask) {
    if ((cm & G_TX_CLAMP) || mask == 0) {
        return GFX_WRAP_CLAMP;
    }
    return (cm & G_TX_MIRROR) ? GFX_WRAP_MIRROR : GFX_WRAP_REPEAT;
}

/* Returns the texture for the given tile, decoding it if it isn't in the cache. */
static GfxTexture *lookup_texture(s32 tileIndex) {
    Tile *tile = &g.tiles[tileIndex & 7];
    TmemLoad *load = &g.tmem[(tile->tmem / 8) & 63];
    u32 width, height, size, hash, i, slot, oldest;
    const u8 *tlut = NULL;
    u8 filter, wrapS, wrapT;
    TexCacheEntry *e;
    u32 *pixels;

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
    hash = sample_hash(load->addr, size);
    if (tlut != NULL) {
        hash ^= sample_hash(tlut, (tile->siz == G_IM_SIZ_4b) ? 32 : 512) * 31;
    }
    filter = ((g.omH & (3 << G_MDSFT_TEXTFILT)) == G_TF_POINT) ? GFX_FILTER_POINT : GFX_FILTER_BILINEAR;
    wrapS = wrap_mode(tile->cms, tile->masks);
    wrapT = wrap_mode(tile->cmt, tile->maskt);

    slot = ((uintptr_t) load->addr >> 3) % TEX_CACHE_SIZE;
    oldest = slot;
    for (i = 0; i < 8; i++) {
        e = &sTexCache[(slot + i) % TEX_CACHE_SIZE];
        if (e->used && e->addr == load->addr && e->width == width && e->height == height && e->fmt == tile->fmt &&
            e->siz == tile->siz && e->tlut == tlut && e->hash == hash && e->lineSwapped == load->lineSwapped) {
            if (e->filter != filter || e->wrapS != wrapS || e->wrapT != wrapT) {
                e->filter = e->tex.filter = filter;
                e->wrapS = e->tex.wrapS = wrapS;
                e->wrapT = e->tex.wrapT = wrapT;
                gfxb_texture_upload(&e->tex);
            }
            e->lastFrame = sFrameCount;
            return &e->tex;
        }
        if (!e->used) {
            oldest = (slot + i) % TEX_CACHE_SIZE;
            break;
        }
        if (e->lastFrame < sTexCache[oldest].lastFrame) {
            oldest = (slot + i) % TEX_CACHE_SIZE;
        }
    }

    e = &sTexCache[oldest];
    if (e->used) {
        gfxb_texture_free(&e->tex);
        plat_free((void *) e->tex.pixels);
    }
    pixels = plat_malloc(width * height * 4);
    if (pixels == NULL) {
        e->used = FALSE;
        return NULL;
    }
    decode_texture(pixels, load, tile, width, height, tlut);
    e->used = TRUE;
    e->addr = load->addr;
    e->tlut = tlut;
    e->hash = hash;
    e->width = width;
    e->height = height;
    e->fmt = tile->fmt;
    e->siz = tile->siz;
    e->lineSwapped = load->lineSwapped;
    e->filter = filter;
    e->wrapS = wrapS;
    e->wrapT = wrapT;
    e->lastFrame = sFrameCount;
    e->tex.width = width;
    e->tex.height = height;
    e->tex.pixels = (const unsigned int *) pixels;
    e->tex.filter = filter;
    e->tex.wrapS = wrapS;
    e->tex.wrapT = wrapT;
    e->tex.backendData = NULL;
    gfxb_texture_upload(&e->tex);
    return &e->tex;
}

/* Converts a texture coordinate in 10.5 texels to the normalised coordinate of the tile's texture. */
static inline float tex_coord(s32 coord, u8 shift, u16 tileStart, u32 size) {
    float c = coord * (1.0f / 32.0f);
    if (shift > 10) {
        c *= (float) (1 << (16 - shift));
    } else if (shift > 0) {
        c /= (float) (1 << shift);
    }
    c -= tileStart * 0.25f;
    return c / (float) size;
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
}

static inline u32 cycle_type(void) {
    return g.omH & (3 << G_MDSFT_CYCLETYPE);
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
    s->primLodFrac = g.primLodFrac;

    // Which textures does the combiner read?
    for (i = 0; i < s->combiner.numCycles; i++) {
        const GfxCCEquation *eqs[2] = { &s->combiner.rgb[i], &s->combiner.alpha[i] };
        s32 j;
        for (j = 0; j < 2; j++) {
            u8 in[4] = { eqs[j]->a, eqs[j]->b, eqs[j]->c, eqs[j]->d };
            s32 k;
            for (k = 0; k < 4; k++) {
                if (in[k] == GFX_CC_TEXEL0 || in[k] == GFX_CC_TEXEL0_ALPHA) {
                    usesTex0 = TRUE;
                }
                if (in[k] == GFX_CC_TEXEL1 || in[k] == GFX_CC_TEXEL1_ALPHA) {
                    usesTex1 = TRUE;
                }
            }
        }
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

    if (usesTex0 || cyc == G_CYC_COPY) {
        s->textures[0] = lookup_texture(isRect ? (g.rdpHalf1 >> 0) & 7 : 0);
    }
    if (usesTex1) {
        s->textures[1] = lookup_texture(isRect ? ((g.rdpHalf1 + 1) & 7) : 1);
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
            trace_state(&sBatchState, sBatchCount);
            plat_log("    v0 %.3f %.3f %.3f %.3f uv %.3f %.3f rgba %d %d %d %d", v->x, v->y, v->z, v->w, v->u0, v->v0,
                     v->r, v->g, v->b, v->a);
        }
        gfxb_draw_triangles(&sBatchState, sBatchVerts, sBatchCount);
        sBatchCount = 0;
    }
}

static GfxVertex *batch_triangle(const GfxDrawState *state) {
    if (sBatchCount > 0 && (sBatchCount >= MAX_BATCH_TRIANGLES || !draw_states_equal(state, &sBatchState))) {
        flush_batch();
    }
    if (sBatchCount == 0) {
        sBatchState = *state;
    }
    return &sBatchVerts[sBatchCount++ * 3];
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

/* Folds the N64 viewport into clip space coordinates (see gfx_backend.h). */
static inline void apply_viewport(float *x, float *y, float w) {
    const float halfW = GFX_SCREEN_WIDTH / 2.0f, halfH = GFX_SCREEN_HEIGHT / 2.0f;
    *x = *x * (g.vpScale[0] / halfW) + w * (g.vpTrans[0] / halfW - 1.0f);
    *y = *y * (g.vpScale[1] / halfH) + w * (1.0f - g.vpTrans[1] / halfH);
}

static void load_vertices(const Vertex *src, s32 count, s32 dest) {
    float(*m)[4] = g.matrices[g.curMatrix];
    s32 i;

    for (i = 0; i < count && dest + i < MAX_VERTICES; i++) {
        RspVertex *v = &g.verts[dest + i];
        float x = src[i].x, y = src[i].y, z = src[i].z;
        v->x = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
        v->y = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
        v->z = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
        v->w = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
        if (g.billboard && dest + i > 0) {
            // Billboard vertices are offsets from the anchor vertex, in clip space.
            v->x += g.verts[0].x;
            v->y += g.verts[0].y;
            v->z += g.verts[0].z;
            v->w += g.verts[0].w;
        }
        v->r = src[i].r;
        v->g = src[i].g;
        v->b = src[i].b;
        v->a = src[i].a;
        // Fog, as the RSP computes it from the normalised depth.
        {
            float w = (v->w != 0.0f) ? v->w : 0.0001f;
            s32 fog = (s32) ((v->z / w) * g.fogMul + g.fogOffset);
            v->fog = (fog < 0) ? 0 : (fog > 255) ? 255 : fog;
        }
    }
}

/* With G_FOG set the RSP replaces the shade alpha with the fog factor. */
static void finish_vertex(GfxVertex *out, const RspVertex *in) {
    out->x = in->x;
    out->y = in->y;
    out->z = in->z;
    out->w = in->w;
    apply_viewport(&out->x, &out->y, in->w);
    out->r = in->r;
    out->g = in->g;
    out->b = in->b;
    out->a = (g.geometryMode & G_FOG) ? in->fog : in->a;
}

static void draw_dkr_triangles(const Triangle *tris, s32 count, s32 textured) {
    GfxDrawState state;
    s32 i, k;
    Tile *tile0 = &g.tiles[0], *tile1 = &g.tiles[1];
    float w0 = 1.0f, h0 = 1.0f, w1 = 1.0f, h1 = 1.0f;

    build_draw_state(&state, FALSE);
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
    for (i = 0; i < count; i++) {
        const Triangle *t = &tris[i];
        const TexCoords *uv[3] = { &t->uv0, &t->uv1, &t->uv2 };
        u8 idx[3] = { t->vi0, t->vi1, t->vi2 };
        GfxVertex *out;

        if (idx[0] >= MAX_VERTICES || idx[1] >= MAX_VERTICES || idx[2] >= MAX_VERTICES) {
            continue;
        }
        // Backface culling: flag 0x40 means double sided. The game mirrors the viewport for mirror mode.
        state.cullMode = (t->flags & BACKFACE_DRAW) ? 0 : (g.vpScale[0] < 0.0f ? 2 : 1);
        out = batch_triangle(&state);
        for (k = 0; k < 3; k++) {
            finish_vertex(&out[k], &g.verts[idx[k]]);
            if (textured) {
                out[k].u0 = tex_coord(uv[k]->u, tile0->shifts, tile0->uls, (u32) w0);
                out[k].v0 = tex_coord(uv[k]->v, tile0->shiftt, tile0->ult, (u32) h0);
                out[k].u1 = tex_coord(uv[k]->u, tile1->shifts, tile1->uls, (u32) w1);
                out[k].v1 = tex_coord(uv[k]->v, tile1->shiftt, tile1->ult, (u32) h1);
            } else {
                out[k].u0 = out[k].v0 = out[k].u1 = out[k].v1 = 0.0f;
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
    GfxVertex *out;
    s32 i;
    float w0 = 1.0f, h0 = 1.0f, w1 = 1.0f, h1 = 1.0f;
    Tile *tile = &g.tiles[g.rdpHalf1 & 7];
    Tile *tile1 = &g.tiles[(g.rdpHalf1 + 1) & 7];

    build_draw_state(&state, TRUE);
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
        corners[i].u0 = (s - tile->uls * 0.25f) / w0;
        corners[i].v0 = (t - tile->ult * 0.25f) / h0;
        corners[i].u1 = (s - tile1->uls * 0.25f) / w1;
        corners[i].v1 = (t - tile1->ult * 0.25f) / h1;
        corners[i].r = corners[i].g = corners[i].b = corners[i].a = 255;
    }
    state.cullMode = 0;
    out = batch_triangle(&state);
    out[0] = corners[0];
    out[1] = corners[2];
    out[2] = corners[1];
    out = batch_triangle(&state);
    out[0] = corners[1];
    out[1] = corners[2];
    out[2] = corners[3];
}

static void texture_rectangle(u32 w0, u32 w1, s32 flip) {
    float lrx = ((w0 >> 12) & 0xFFF) * 0.25f;
    float lry = (w0 & 0xFFF) * 0.25f;
    float ulx = ((w1 >> 12) & 0xFFF) * 0.25f;
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
    // Clip to the scissor.
    if (rect[0] < g.scissor[0]) rect[0] = g.scissor[0];
    if (rect[1] < g.scissor[1]) rect[1] = g.scissor[1];
    if (rect[2] > g.scissor[2]) rect[2] = g.scissor[2];
    if (rect[3] > g.scissor[3]) rect[3] = g.scissor[3];
    if (rect[2] <= rect[0] || rect[3] <= rect[1]) {
        return;
    }
    if (cyc == G_CYC_FILL) {
        flush_batch();
        if (g.colorImage == g.depthImage) {
            gfxb_clear_depth(rect);
        } else {
            u32 c = rgba5551_to_8888(g.fillColor >> 16);
            gfxb_clear_color(rect, c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, 255);
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

static void run_command_list(const Gfx *dl, s32 maxCommands, s32 depth) {
    s32 executed = 0;

    while (maxCommands < 0 || executed < maxCommands) {
        u32 w0 = dl->words.w0;
        u32 w1 = dl->words.w1;
        u32 op = w0 >> 24;
        dl++;
        executed++;
        TRACE("%*s%08X %08X", depth * 2, "", (unsigned) w0, (unsigned) w1);

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

/******************************** Entry points ********************************/

static s32 sGfxInitialised;

void port_gfx_init(void) {
    if (!sGfxInitialised) {
        const char *trace = plat_getenv("DKR_TRACE_FRAME");
        sGfxInitialised = TRUE;
        sTraceFrame = (trace != NULL) ? port_atoi(trace) : -1;
        gfxb_init();
    }
}

void port_gfx_run_task(void *taskPtr) {
    OSTask *task = (OSTask *) taskPtr;
    port_gfx_init();
    sFrameCount++;
    gfxb_start_frame();
    g.vertexIndex = 0;
    g.billboard = 0;
    if (g.scissor[2] == 0) {
        g.scissor[2] = GFX_SCREEN_WIDTH;
        g.scissor[3] = GFX_SCREEN_HEIGHT;
    }
    run_dl((const Gfx *) task->t.data_ptr, -1);
    flush_batch();
    gfxb_end_frame();
}
