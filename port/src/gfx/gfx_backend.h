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

/* A texture prepared by the frontend. Pixels are RGBA8888 (R in the lowest byte). */
typedef struct GfxTexture {
    unsigned int width, height;
    const unsigned int *pixels;
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

#endif
