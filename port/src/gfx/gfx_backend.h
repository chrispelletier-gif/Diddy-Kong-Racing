#ifndef PORT_GFX_BACKEND_H
#define PORT_GFX_BACKEND_H

/*
 * Interface between the F3DDKR/RDP interpreter (gfx_rdp.c) and a rendering backend.
 * Only builtin C types are used here, so backends can be written against their platform's own headers.
 *
 * Coordinate conventions:
 *  - Vertex positions are clip space coordinates for a 320x240 "N64 screen": x/w and y/w are in [-1, 1]
 *    with +y up, z/w is in [-1, 1] with -1 nearest. The N64 viewport transform is already applied.
 *  - Texture coordinates are normalised (1.0 = one full texture width/height, before wrapping).
 *  - Rectangles (scissor, fills) are in N64 screen pixels, origin top left.
 */


#define GFX_SCREEN_WIDTH 320
#define GFX_SCREEN_HEIGHT 240

/* Colour combiner inputs, matching the RDP's sources. */
enum GfxCCInput {
    GFX_CC_ZERO,
    GFX_CC_ONE,
    GFX_CC_COMBINED,
    GFX_CC_TEXEL0,
    GFX_CC_TEXEL1,
    GFX_CC_PRIM,
    GFX_CC_SHADE,
    GFX_CC_ENV,
    GFX_CC_COMBINED_ALPHA,
    GFX_CC_TEXEL0_ALPHA,
    GFX_CC_TEXEL1_ALPHA,
    GFX_CC_PRIM_ALPHA,
    GFX_CC_SHADE_ALPHA,
    GFX_CC_ENV_ALPHA,
    GFX_CC_LOD_FRACTION,
    GFX_CC_PRIM_LOD_FRACTION,
    GFX_CC_NOISE,
    GFX_CC_KEY_CENTER,
    GFX_CC_KEY_SCALE,
    GFX_CC_K4,
    GFX_CC_K5,
    GFX_CC_INPUT_COUNT
};

/* One (a - b) * c + d equation. */
typedef struct GfxCCEquation {
    unsigned char a, b, c, d;
} GfxCCEquation;

/* A full combiner: up to two cycles, separate colour and alpha equations. */
typedef struct GfxCombiner {
    GfxCCEquation rgb[2];
    GfxCCEquation alpha[2];
    unsigned char numCycles;
} GfxCombiner;

enum GfxBlendMode {
    GFX_BLEND_OPAQUE,
    GFX_BLEND_ALPHA,    /* src * a + dst * (1 - a) */
    GFX_BLEND_ADD,      /* src * a + dst */
    GFX_BLEND_ALPHA_CVG /* opaque, alpha tested (cutout) */
};

enum GfxTextureFilter { GFX_FILTER_POINT, GFX_FILTER_BILINEAR };
enum GfxTextureWrap { GFX_WRAP_REPEAT, GFX_WRAP_MIRROR, GFX_WRAP_CLAMP };

/*
 * The smallest format that holds a texture's pixels exactly, picked from the N64 format it was decoded from. The
 * 3DS renderer stores the texture on the GPU in it (roadmap item P7): 16 or 8 bits a texel instead of 32, so the GPU
 * reads less memory. RGBA5551 and LA4 channels are widened by repeating their bits, like the N64 does.
 */
enum GfxTextureFormat {
    GFX_TEXFMT_RGBA8,    /* 32 bit RGBA and anything else */
    GFX_TEXFMT_RGBA5551, /* RGBA16, and CI textures with an RGBA16 palette */
    GFX_TEXFMT_LA8,      /* IA16, IA4, I8 (intensity and alpha the same), CI4 with an IA16 palette */
    GFX_TEXFMT_LA4       /* IA8, I4 */
};

/* A texture prepared by the frontend. Pixels are RGBA8888 (R in the lowest byte). */
typedef struct GfxTexture {
    unsigned int width, height;
    const unsigned int *pixels;
    unsigned char format; /* enum GfxTextureFormat: the pixels fit it exactly */
    unsigned char filter;
    unsigned char wrapS, wrapT;
    void *backendData; /* Owned by the backend. */
} GfxTexture;

typedef struct GfxVertex {
    float x, y, z, w;
    float u0, v0;
    float u1, v1;
    unsigned char r, g, b, a; /* Shade colour. When fog is on, a holds the fog factor (0 = no fog). */
} GfxVertex;

typedef struct GfxDrawState {
    GfxCombiner combiner;
    unsigned char prim[4];
    unsigned char env[4];
    unsigned char fog[4];
    unsigned char primLodFrac;
    unsigned char blend;            /* enum GfxBlendMode */
    unsigned char alphaTest;        /* Discard pixels with alpha below alphaRef */
    unsigned char alphaRef;
    unsigned char depthTest;
    unsigned char depthWrite;
    unsigned char decal;            /* Pull towards the camera to win depth fights (ZMODE_DEC) */
    unsigned char fogEnabled;       /* Blend towards fog colour using vertex alpha */
    short fogMul, fogOffset;        /* Fog factor = clamp(z / w * fogMul + fogOffset, 0, 255), for per pixel fog */
    unsigned char cullMode;         /* 0 none, 1 back faces, 2 front faces (in screen space, CCW front) */
    GfxTexture *textures[2];
    short scissor[4];       /* x0, y0, x1, y1 (exclusive) */
} GfxDrawState;

/* Backend entry points. */
void gfxb_init(void);
void gfxb_start_frame(void);
void gfxb_end_frame(void);
void gfxb_clear_color(const short rect[4], unsigned char r, unsigned char g, unsigned char b, unsigned char a);
void gfxb_clear_depth(const short rect[4]);
void gfxb_draw_triangles(const GfxDrawState *state, const GfxVertex *verts, int numTriangles);
void gfxb_texture_upload(GfxTexture *tex);
void gfxb_texture_free(GfxTexture *tex);
/* Makes dst (a copy of src with another filter) draw from src's GPU copy instead of uploading its own. Both are freed
 * with gfxb_texture_free, together. */
void gfxb_texture_share(GfxTexture *dst, const GfxTexture *src);

/* A pixel (RGBA8888, R in the lowest byte) in a texture format's bits, and back the way the 3DS GPU widens them. */
static inline unsigned int gfx_texel_encode(unsigned int c, int format) {
    unsigned int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
    switch (format) {
        case GFX_TEXFMT_RGBA5551:
            return ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7);
        case GFX_TEXFMT_LA8:
            return (r << 8) | a;
        case GFX_TEXFMT_LA4:
            return ((r >> 4) << 4) | (a >> 4);
        default:
            return (r << 24) | (g << 16) | (b << 8) | a; /* The GPU's RGBA8 has R in the highest byte */
    }
}

static inline unsigned int gfx_texel_decode(unsigned int v, int format) {
    unsigned int r, g, b, a;
    switch (format) {
        case GFX_TEXFMT_RGBA5551:
            r = (v >> 11) & 0x1F;
            g = (v >> 6) & 0x1F;
            b = (v >> 1) & 0x1F;
            r = (r << 3) | (r >> 2);
            g = (g << 3) | (g >> 2);
            b = (b << 3) | (b >> 2);
            a = (v & 1) ? 255 : 0;
            break;
        case GFX_TEXFMT_LA8:
            r = g = b = (v >> 8) & 0xFF;
            a = v & 0xFF;
            break;
        case GFX_TEXFMT_LA4:
            r = g = b = ((v >> 4) & 0xF) * 17;
            a = (v & 0xF) * 17;
            break;
        default:
            r = v >> 24;
            g = (v >> 16) & 0xFF;
            b = (v >> 8) & 0xFF;
            a = v & 0xFF;
            break;
    }
    return r | (g << 8) | (b << 16) | (a << 24);
}

/* Bytes per texel. */
static inline unsigned int gfx_texel_bytes(int format) {
    return (format == GFX_TEXFMT_RGBA8) ? 4 : (format == GFX_TEXFMT_LA4) ? 1 : 2;
}

/*
 * Whether a texture gets mipmaps (roadmap item G1): bilinear, at least 16x16, and either repeating on an axis (ground,
 * water) or opaque. Clamped opaque textures are mostly walls and cliffs, which DKR cuts into one texture per tile.
 * Clamped textures with transparency (kart shadows, decals) keep one level: their shape runs out to the edges, so
 * small levels have see-through edges that aren't, and a shadow seen at a low angle came out as a dark rectangle.
 */
static inline int gfx_wants_mipmaps(const GfxTexture *tex) {
    unsigned int i, n = tex->width * tex->height;
    if (tex->filter == GFX_FILTER_POINT || tex->width < 16 || tex->height < 16) {
        return 0;
    }
    if (tex->wrapS != GFX_WRAP_CLAMP || tex->wrapT != GFX_WRAP_CLAMP) {
        return 1;
    }
    for (i = 0; i < n; i++) {
        if ((tex->pixels[i] >> 24) != 0xFF) {
            return 0;
        }
    }
    return 1;
}

/*
 * The next smaller mipmap level (roadmap item G1): each 2x2 block of src (w x h, RGBA8888) averaged into one pixel of
 * dst ((w / 2) x (h / 2), which may be src itself). Colours are weighted by alpha, so the colour hidden under
 * transparent texels (often black) doesn't darken the edges of cutouts such as trees and fences.
 */
static inline void gfx_mip_downsample(const unsigned int *src, unsigned int w, unsigned int h, unsigned int *dst) {
    unsigned int x, y, i;
    for (y = 0; y < h / 2; y++) {
        for (x = 0; x < w / 2; x++) {
            unsigned int c[4], sum[3] = { 0, 0, 0 }, plain[3] = { 0, 0, 0 }, alpha = 0, k;
            c[0] = src[(2 * y) * w + 2 * x];
            c[1] = src[(2 * y) * w + 2 * x + 1];
            c[2] = src[(2 * y + 1) * w + 2 * x];
            c[3] = src[(2 * y + 1) * w + 2 * x + 1];
            for (i = 0; i < 4; i++) {
                unsigned int a = c[i] >> 24;
                for (k = 0; k < 3; k++) {
                    unsigned int v = (c[i] >> (8 * k)) & 0xFF;
                    sum[k] += v * a;
                    plain[k] += v;
                }
                alpha += a;
            }
            for (k = 0; k < 3; k++) {
                sum[k] = alpha > 0 ? (sum[k] + alpha / 2) / alpha : (plain[k] + 2) / 4;
            }
            dst[y * (w / 2) + x] = sum[0] | (sum[1] << 8) | (sum[2] << 16) | (((alpha + 2) / 4) << 24);
        }
    }
}

#endif
