/*
 * Software rendering backend for the host build. It evaluates the RDP colour combiner exactly, which makes it a
 * reference to compare the 3DS GPU backend against, and it can dump frames to PNG files for testing.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx_backend.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_FILES_UNUSED
#include "../../tools/stb/stb_image_write.h"

#define H GFX_SCREEN_HEIGHT
/* The frame's width: 320, or 400 for a preview of the 3DS's widescreen (DKR_WIDESCREEN=1, roadmap item G3; the game's
 * camera is widened by plat_aspect_scale, flat draws are kept 4:3 as gfx_flat_x says). */
#define MAX_W 400
#define NARROW (320.0f / 400.0f)
static int W = GFX_SCREEN_WIDTH;
static int sWide;

static uint8_t sColor[MAX_W * H * 4];
static float sDepth[MAX_W * H];
static volatile int sFramesRendered;
static int sMipmaps = -1, sColour = -1; // DKR_MIPMAP, DKR_COLOUR: see read_upgrade_settings
static int sDumpEvery = -1;
static const char *sDumpDir;

int host_frames_rendered(void) {
    return sFramesRendered;
}

static void read_upgrade_settings(void);
static void build_mips(GfxTexture *tex);

void gfxb_init(void) {
    const char *env = getenv("DKR_DUMP_EVERY");
    read_upgrade_settings();
    sWide = getenv("DKR_WIDESCREEN") != NULL && atoi(getenv("DKR_WIDESCREEN")) != 0;
    W = sWide ? MAX_W : GFX_SCREEN_WIDTH;
    sDumpEvery = env ? atoi(env) : 0;
    sDumpDir = getenv("DKR_DUMP_DIR");
    if (sDumpDir == NULL) {
        sDumpDir = ".";
    }
}

void gfxb_start_frame(void) {
}

void gfxb_end_frame(void) {
    sFramesRendered++;
    if (sDumpEvery > 0 && sFramesRendered % sDumpEvery == 0) {
        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%05d.png", sDumpDir, sFramesRendered);
        stbi_write_png(path, W, H, 4, sColor, W * 4);
    }
}

/* Writes the current frame (320x240, or 400x240 in widescreen) to a PNG file. */
void gfxb_soft_save_png(const char *path) {
    stbi_write_png(path, W, H, 4, sColor, W * 4);
}

/* A rectangle's x in N64 screen pixels, on this frame: a flat draw's (fills and scissors of flat draws) or a 3D one's. */
static int frame_x(int x, int flat) {
    if (!sWide) {
        return x;
    }
    return (int) floorf((flat ? gfx_flat_px(x, NARROW, W) : x * ((float) W / GFX_SCREEN_WIDTH)) + 0.5f);
}

static void clip_rect(const int16_t in[4], int *x0, int *y0, int *x1, int *y1) {
    *x0 = frame_x(in[0], 1);
    *x1 = frame_x(in[2], 1);
    *x0 = *x0 < 0 ? 0 : *x0;
    *y0 = in[1] < 0 ? 0 : in[1];
    *x1 = *x1 > W ? W : *x1;
    *y1 = in[3] > H ? H : in[3];
}

void gfxb_clear_color(const int16_t rect[4], uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    int x, y, x0, y0, x1, y1;
    clip_rect(rect, &x0, &y0, &x1, &y1);
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            uint8_t *p = &sColor[(y * W + x) * 4];
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = a;
        }
    }
}

void gfxb_clear_depth(const int16_t rect[4]) {
    int x, y, x0, y0, x1, y1;
    clip_rect(rect, &x0, &y0, &x1, &y1);
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            sDepth[y * W + x] = 1.0f;
        }
    }
}

void gfxb_texture_upload(GfxTexture *tex) {
    static int count, sMismatches;
    const char *dir = getenv("DKR_DUMP_TEXTURES");
    unsigned int i, n = tex->width * tex->height;
    // The 3DS renderer stores the texture in tex->format: check that it holds every pixel exactly.
    for (i = 0; i < n; i++) {
        unsigned int c = tex->pixels[i];
        if (gfx_texel_decode(gfx_texel_encode(c, tex->format), tex->format) != c) {
            if (sMismatches++ < 20) {
                fprintf(stderr, "texture %ux%u: pixel %u (%08X) doesn't fit texture format %d\n", tex->width,
                        tex->height, i, c, tex->format);
            }
            break;
        }
    }
    build_mips(tex);
    if (dir != NULL && count < 2000) {
        char path[512];
        snprintf(path, sizeof(path), "%s/tex_%04d_%ux%u_f%d.png", dir, count++, tex->width, tex->height,
                 sFramesRendered);
        stbi_write_png(path, tex->width, tex->height, 4, tex->pixels, tex->width * 4);
    }
}

/*
 * Mipmaps and the colour boost of the 3DS renderer (roadmap items G1 and G2), for previewing them: DKR_MIPMAP=1 and
 * DKR_COLOUR=1 to 3. Both are off by default, so frames stay comparable with the N64 and with earlier commits. The
 * level is picked as the 3DS GPU does without anti-aliasing (with it, the GPU uses one level sharper).
 */
#define MAX_MIP_LEVELS 8

typedef struct SoftMips {
    int levels; // After the first, which is the texture's own pixels
    unsigned int w[MAX_MIP_LEVELS + 1], h[MAX_MIP_LEVELS + 1];
    unsigned int *pixels[MAX_MIP_LEVELS + 1];
} SoftMips;

static void read_upgrade_settings(void) {
    if (sMipmaps < 0) {
        const char *env = getenv("DKR_MIPMAP");
        sMipmaps = env != NULL && atoi(env) != 0;
        env = getenv("DKR_COLOUR");
        sColour = env != NULL ? atoi(env) : 0;
        sColour = sColour < 0 ? 0 : sColour > 3 ? 3 : sColour;
    }
}

static void free_mips(GfxTexture *tex) {
    SoftMips *m;
    int i;
    read_upgrade_settings();
    if (!sMipmaps || (m = (SoftMips *) tex->backendData) == NULL) {
        return; // backendData isn't ours (the 3DS's reference renderer shares textures with the GPU one).
    }
    for (i = 1; i <= m->levels; i++) {
        free(m->pixels[i]);
    }
    free(m);
    tex->backendData = NULL;
}

/* Like the 3DS (gfx_wants_mipmaps), halved down to 8 on the short side while the size stays even. */
static void build_mips(GfxTexture *tex) {
    SoftMips *m;
    unsigned int w = tex->width, h = tex->height;
    read_upgrade_settings();
    free_mips(tex);
    if (!sMipmaps || !gfx_wants_mipmaps(tex)) {
        return;
    }
    m = calloc(1, sizeof(SoftMips));
    m->w[0] = w;
    m->h[0] = h;
    m->pixels[0] = (unsigned int *) tex->pixels;
    while (m->levels < MAX_MIP_LEVELS && w % 2 == 0 && h % 2 == 0 && w >= 16 && h >= 16) {
        unsigned int *next = malloc((w / 2) * (h / 2) * sizeof(unsigned int));
        gfx_mip_downsample(m->pixels[m->levels], w, h, next);
        w /= 2;
        h /= 2;
        m->levels++;
        m->pixels[m->levels] = next;
        m->w[m->levels] = w;
        m->h[m->levels] = h;
    }
    tex->backendData = m;
}

void gfxb_texture_free(GfxTexture *tex) {
    free_mips(tex);
}

void gfxb_texture_share(GfxTexture *dst, const GfxTexture *src) {
    (void) src;
    build_mips(dst);
}

/******************************** Texturing ********************************/

static inline int wrap_coord(int c, int size, int mode) {
    switch (mode) {
        case GFX_WRAP_CLAMP:
            return c < 0 ? 0 : (c >= size ? size - 1 : c);
        case GFX_WRAP_MIRROR: {
            int period = size * 2;
            c %= period;
            if (c < 0) {
                c += period;
            }
            return c < size ? c : period - 1 - c;
        }
        default:
            c %= size;
            return c < 0 ? c + size : c;
    }
}

static void sample_level(const GfxTexture *tex, const unsigned int *px, int w, int h, float u, float v,
                         float out[4]) {
    if (tex->filter == GFX_FILTER_POINT) {
        int x = wrap_coord((int) floorf(u * w), w, tex->wrapS);
        int y = wrap_coord((int) floorf(v * h), h, tex->wrapT);
        uint32_t c = px[y * w + x];
        out[0] = (c & 0xFF) / 255.0f;
        out[1] = ((c >> 8) & 0xFF) / 255.0f;
        out[2] = ((c >> 16) & 0xFF) / 255.0f;
        out[3] = (c >> 24) / 255.0f;
    } else {
        float fx = u * w - 0.5f, fy = v * h - 0.5f;
        int x0 = (int) floorf(fx), y0 = (int) floorf(fy);
        float ax = fx - x0, ay = fy - y0;
        int xs[2] = { wrap_coord(x0, w, tex->wrapS), wrap_coord(x0 + 1, w, tex->wrapS) };
        int ys[2] = { wrap_coord(y0, h, tex->wrapT), wrap_coord(y0 + 1, h, tex->wrapT) };
        int i, j, k;
        for (k = 0; k < 4; k++) {
            out[k] = 0.0f;
        }
        for (j = 0; j < 2; j++) {
            for (i = 0; i < 2; i++) {
                uint32_t c = px[ys[j] * w + xs[i]];
                float weight = (i ? ax : 1.0f - ax) * (j ? ay : 1.0f - ay);
                out[0] += (c & 0xFF) / 255.0f * weight;
                out[1] += ((c >> 8) & 0xFF) / 255.0f * weight;
                out[2] += ((c >> 16) & 0xFF) / 255.0f * weight;
                out[3] += (c >> 24) / 255.0f * weight;
            }
        }
    }
}

/* Samples tex at (u, v), where a pixel step moves the coordinates by (dudx, dvdx) and (dudy, dvdy). */
static void sample(const GfxTexture *tex, float u, float v, float dudx, float dvdx, float dudy, float dvdy,
                   float out[4]) {
    const SoftMips *m = sMipmaps > 0 ? (const SoftMips *) tex->backendData : NULL;
    float lod, x, y, rho, frac, other[4];
    int level, k;

    if (m == NULL) {
        sample_level(tex, tex->pixels, tex->width, tex->height, u, v, out);
        return;
    }
    x = sqrtf(dudx * dudx * tex->width * tex->width + dvdx * dvdx * tex->height * tex->height);
    y = sqrtf(dudy * dudy * tex->width * tex->width + dvdy * dvdy * tex->height * tex->height);
    rho = x > y ? x : y;
    lod = rho > 1.0f ? log2f(rho) : 0.0f;
    if (lod >= m->levels) {
        lod = m->levels;
    }
    level = (int) lod;
    frac = lod - level;
    sample_level(tex, m->pixels[level], m->w[level], m->h[level], u, v, out);
    if (frac > 0.0f && level < m->levels) {
        sample_level(tex, m->pixels[level + 1], m->w[level + 1], m->h[level + 1], u, v, other);
        for (k = 0; k < 4; k++) {
            out[k] += (other[k] - out[k]) * frac;
        }
    }
}

/******************************** Combiner ********************************/

typedef struct CCInputs {
    float combined[4];
    float tex0[4];
    float tex1[4];
    float shade[4];
    float prim[4];
    float env[4];
    float primLod;
} CCInputs;

static float cc_input(const CCInputs *in, int input, int channel) {
    switch (input) {
        case GFX_CC_ZERO: return 0.0f;
        case GFX_CC_ONE: return 1.0f;
        case GFX_CC_COMBINED: return in->combined[channel];
        case GFX_CC_TEXEL0: return in->tex0[channel];
        case GFX_CC_TEXEL1: return in->tex1[channel];
        case GFX_CC_PRIM: return in->prim[channel];
        case GFX_CC_SHADE: return in->shade[channel];
        case GFX_CC_ENV: return in->env[channel];
        case GFX_CC_COMBINED_ALPHA: return in->combined[3];
        case GFX_CC_TEXEL0_ALPHA: return in->tex0[3];
        case GFX_CC_TEXEL1_ALPHA: return in->tex1[3];
        case GFX_CC_PRIM_ALPHA: return in->prim[3];
        case GFX_CC_SHADE_ALPHA: return in->shade[3];
        case GFX_CC_ENV_ALPHA: return in->env[3];
        case GFX_CC_PRIM_LOD_FRACTION: return in->primLod;
        case GFX_CC_LOD_FRACTION: return 0.0f;
        case GFX_CC_NOISE: return (rand() & 0xFF) / 255.0f;
        default: return 0.0f;
    }
}

static float cc_eval(const CCInputs *in, const GfxCCEquation *eq, int channel) {
    float v = (cc_input(in, eq->a, channel) - cc_input(in, eq->b, channel)) * cc_input(in, eq->c, channel) +
              cc_input(in, eq->d, channel);
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/******************************** Rasterizer ********************************/

typedef struct ClipVert {
    float x, y, z, w;
    float u0, v0, u1, v1;
    float r, g, b, a;
} ClipVert;

static void to_clip(ClipVert *o, const GfxVertex *v, int flat) {
    o->x = (sWide && flat) ? gfx_flat_x(v->x, v->w, NARROW, flat == GFX_FLAT_STRETCH) : v->x;
    o->y = v->y;
    o->z = v->z;
    o->w = v->w;
    o->u0 = v->u0;
    o->v0 = v->v0;
    o->u1 = v->u1;
    o->v1 = v->v1;
    o->r = v->r / 255.0f;
    o->g = v->g / 255.0f;
    o->b = v->b / 255.0f;
    o->a = v->a / 255.0f;
}

static ClipVert lerp_vert(const ClipVert *a, const ClipVert *b, float t) {
    ClipVert o;
    const float *pa = (const float *) a, *pb = (const float *) b;
    float *po = (float *) &o;
    int i;
    for (i = 0; i < (int) (sizeof(ClipVert) / sizeof(float)); i++) {
        po[i] = pa[i] + (pb[i] - pa[i]) * t;
    }
    return o;
}

/* Clips a polygon against z > -w (the near plane). Returns the new vertex count. */
static int clip_near(ClipVert *in, int count, ClipVert *out) {
    int i, n = 0;
    for (i = 0; i < count; i++) {
        const ClipVert *a = &in[i], *b = &in[(i + 1) % count];
        float da = a->z + a->w, db = b->z + b->w;
        if (da >= 0.0f) {
            out[n++] = *a;
        }
        if ((da >= 0.0f) != (db >= 0.0f)) {
            out[n++] = lerp_vert(a, b, da / (da - db));
        }
    }
    return n;
}

typedef struct ScreenVert {
    float x, y, z, invW;
    float u0w, v0w, u1w, v1w;
    float rw, gw, bw, aw;
} ScreenVert;

static void project(ScreenVert *s, const ClipVert *c) {
    float invW = 1.0f / c->w;
    s->x = (c->x * invW + 1.0f) * (W / 2.0f);
    s->y = (1.0f - c->y * invW) * (H / 2.0f);
    s->z = c->z * invW * 0.5f + 0.5f;
    s->invW = invW;
    s->u0w = c->u0 * invW;
    s->v0w = c->v0 * invW;
    s->u1w = c->u1 * invW;
    s->v1w = c->v1 * invW;
    s->rw = c->r * invW;
    s->gw = c->g * invW;
    s->bw = c->b * invW;
    s->aw = c->a * invW;
}

/* The perspective correct texture coordinates of the given unit for barycentric weights w. */
static void tex_coords(const ScreenVert *v0, const ScreenVert *v1, const ScreenVert *v2, const float w[3], int unit,
                       float *u, float *v) {
    float persp = 1.0f / (w[0] * v0->invW + w[1] * v1->invW + w[2] * v2->invW);
    if (unit == 0) {
        *u = (w[0] * v0->u0w + w[1] * v1->u0w + w[2] * v2->u0w) * persp;
        *v = (w[0] * v0->v0w + w[1] * v1->v0w + w[2] * v2->v0w) * persp;
    } else {
        *u = (w[0] * v0->u1w + w[1] * v1->u1w + w[2] * v2->u1w) * persp;
        *v = (w[0] * v0->v1w + w[1] * v1->v1w + w[2] * v2->v1w) * persp;
    }
}

static void sample_at(const GfxTexture *tex, const ScreenVert *v0, const ScreenVert *v1, const ScreenVert *v2,
                      float w0, float w1, float w2, const float dw[2][3], int unit, float out[4]) {
    float w[3] = { w0, w1, w2 }, u, v, du[2] = { 0.0f, 0.0f }, dv[2] = { 0.0f, 0.0f };
    int axis, i;
    tex_coords(v0, v1, v2, w, unit, &u, &v);
    if (sMipmaps > 0 && tex->backendData != NULL) {
        for (axis = 0; axis < 2; axis++) {
            float step[3], su, sv;
            for (i = 0; i < 3; i++) {
                step[i] = w[i] + dw[axis][i];
            }
            tex_coords(v0, v1, v2, step, unit, &su, &sv);
            du[axis] = su - u;
            dv[axis] = sv - v;
        }
    }
    sample(tex, u, v, du[0], dv[0], du[1], dv[1], out);
}

static void raster_triangle(const GfxDrawState *st, const ScreenVert *v0, const ScreenVert *v1, const ScreenVert *v2) {
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    int minX, maxX, minY, maxY, x, y, k;
    float dw[2][3];
    CCInputs in;

    if (area == 0.0f) {
        return;
    }
    // Screen y points down, so counter clockwise triangles have a negative area here.
    if (st->cullMode == 1 && area > 0.0f) {
        return;
    }
    if (st->cullMode == 2 && area < 0.0f) {
        return;
    }
    minX = (int) floorf(fminf(v0->x, fminf(v1->x, v2->x)));
    maxX = (int) ceilf(fmaxf(v0->x, fmaxf(v1->x, v2->x)));
    minY = (int) floorf(fminf(v0->y, fminf(v1->y, v2->y)));
    maxY = (int) ceilf(fmaxf(v0->y, fmaxf(v1->y, v2->y)));
    if (minX < st->scissor[0]) minX = st->scissor[0];
    if (minY < st->scissor[1]) minY = st->scissor[1];
    if (maxX > st->scissor[2]) maxX = st->scissor[2];
    if (maxY > st->scissor[3]) maxY = st->scissor[3];
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > W) maxX = W;
    if (maxY > H) maxY = H;

    for (k = 0; k < 4; k++) {
        in.prim[k] = st->prim[k] / 255.0f;
        in.env[k] = st->env[k] / 255.0f;
    }
    in.primLod = st->primLodFrac / 255.0f;
    // How the barycentric weights change a pixel to the right and a pixel down, for picking mipmap levels.
    dw[0][0] = (v1->y - v2->y) / area;
    dw[0][1] = (v2->y - v0->y) / area;
    dw[0][2] = -dw[0][0] - dw[0][1];
    dw[1][0] = (v2->x - v1->x) / area;
    dw[1][1] = (v0->x - v2->x) / area;
    dw[1][2] = -dw[1][0] - dw[1][1];

    for (y = minY; y < maxY; y++) {
        for (x = minX; x < maxX; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((v1->x - px) * (v2->y - py) - (v2->x - px) * (v1->y - py)) / area;
            float w1 = ((v2->x - px) * (v0->y - py) - (v0->x - px) * (v2->y - py)) / area;
            float w2 = 1.0f - w0 - w1;
            float z, invW, persp, out[4], fogAmount;
            uint8_t *dst;
            int i, c;

            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                continue;
            }
            z = w0 * v0->z + w1 * v1->z + w2 * v2->z;
            if (st->decal) {
                z -= 0.0005f;
            }
            if (st->depthTest && z > sDepth[y * W + x]) {
                continue;
            }
            invW = w0 * v0->invW + w1 * v1->invW + w2 * v2->invW;
            persp = 1.0f / invW;
            in.shade[0] = (w0 * v0->rw + w1 * v1->rw + w2 * v2->rw) * persp;
            in.shade[1] = (w0 * v0->gw + w1 * v1->gw + w2 * v2->gw) * persp;
            in.shade[2] = (w0 * v0->bw + w1 * v1->bw + w2 * v2->bw) * persp;
            in.shade[3] = (w0 * v0->aw + w1 * v1->aw + w2 * v2->aw) * persp;
            if (st->textures[0] != NULL) {
                sample_at(st->textures[0], v0, v1, v2, w0, w1, w2, dw, 0, in.tex0);
            } else {
                in.tex0[0] = in.tex0[1] = in.tex0[2] = in.tex0[3] = 1.0f;
            }
            if (st->textures[1] != NULL) {
                sample_at(st->textures[1], v0, v1, v2, w0, w1, w2, dw, 1, in.tex1);
            } else {
                for (k = 0; k < 4; k++) {
                    in.tex1[k] = in.tex0[k];
                }
            }
            for (k = 0; k < 4; k++) {
                in.combined[k] = 0.0f;
            }
            for (c = 0; c < st->combiner.numCycles; c++) {
                for (i = 0; i < 3; i++) {
                    out[i] = cc_eval(&in, &st->combiner.rgb[c], i);
                }
                out[3] = cc_eval(&in, &st->combiner.alpha[c], 3);
                for (k = 0; k < 4; k++) {
                    in.combined[k] = out[k];
                }
            }
            if (sColour > 0) {
                static const float sStrength[4] = { 0.0f, 0.15f, 0.3f, 0.5f };
                float luma = 0.299f * out[0] + 0.587f * out[1] + 0.114f * out[2], t = sStrength[sColour];
                for (k = 0; k < 3; k++) {
                    out[k] += t * (out[k] - luma);
                    out[k] = out[k] < 0.0f ? 0.0f : out[k] > 1.0f ? 1.0f : out[k];
                }
            }
            if (st->alphaTest && out[3] * 255.0f < st->alphaRef) {
                continue;
            }
            dst = &sColor[(y * W + x) * 4];
            if (st->fogEnabled) {
                // Per pixel, from depth, like the 3DS renderer: interpolating the per vertex factor goes wrong for
                // triangles that cross the near plane (DKR_VERTEX_FOG=1 uses the vertex factor anyway).
                static int sVertexFog = -1;
                if (sVertexFog < 0) {
                    sVertexFog = getenv("DKR_VERTEX_FOG") != NULL;
                }
                if (sVertexFog) {
                    fogAmount = in.shade[3];
                } else {
                    float zw = (w0 * v0->z + w1 * v1->z + w2 * v2->z) * 2.0f - 1.0f;
                    float f = zw * st->fogMul + st->fogOffset;
                    fogAmount = ((f < 0.0f) ? 0.0f : (f > 255.0f) ? 255.0f : f) / 255.0f;
                }
                for (k = 0; k < 3; k++) {
                    out[k] = out[k] + (st->fog[k] / 255.0f - out[k]) * fogAmount;
                }
            }
            switch (st->blend) {
                case GFX_BLEND_ALPHA:
                    for (k = 0; k < 3; k++) {
                        float d = dst[k] / 255.0f;
                        out[k] = out[k] * out[3] + d * (1.0f - out[3]);
                    }
                    break;
                case GFX_BLEND_ADD:
                    for (k = 0; k < 3; k++) {
                        float d = dst[k] / 255.0f + out[k] * out[3];
                        out[k] = d > 1.0f ? 1.0f : d;
                    }
                    break;
                default:
                    break;
            }
            for (k = 0; k < 3; k++) {
                dst[k] = (uint8_t) (out[k] * 255.0f + 0.5f);
            }
            dst[3] = 255;
            if (st->depthWrite) {
                sDepth[y * W + x] = z;
            }
        }
    }
}

static void log_combiner(const GfxDrawState *st) {
    static unsigned int seen[512];
    static int numSeen;
    static FILE *f;
    const GfxCombiner *c = &st->combiner;
    unsigned int key = 0;
    int i;
    if (getenv("DKR_LOG_COMBINERS") == NULL) {
        return;
    }
    for (i = 0; i < (int) sizeof(GfxCombiner); i++) {
        key = key * 31 + ((const unsigned char *) c)[i];
    }
    key = key * 31 + st->fogEnabled * 7 + st->blend * 3 + (st->textures[0] != NULL) + (st->textures[1] != NULL) * 2;
    for (i = 0; i < numSeen; i++) {
        if (seen[i] == key) {
            return;
        }
    }
    if (numSeen < 512) {
        seen[numSeen++] = key;
    }
    if (f == NULL) {
        f = fopen(getenv("DKR_LOG_COMBINERS"), "w");
    }
    fprintf(f, "cyc%d rgb0(%d,%d,%d,%d) a0(%d,%d,%d,%d) rgb1(%d,%d,%d,%d) a1(%d,%d,%d,%d) fog%d blend%d tex%d%d at%d\n",
            c->numCycles, c->rgb[0].a, c->rgb[0].b, c->rgb[0].c, c->rgb[0].d, c->alpha[0].a, c->alpha[0].b,
            c->alpha[0].c, c->alpha[0].d, c->rgb[1].a, c->rgb[1].b, c->rgb[1].c, c->rgb[1].d, c->alpha[1].a,
            c->alpha[1].b, c->alpha[1].c, c->alpha[1].d, st->fogEnabled, st->blend, st->textures[0] != NULL,
            st->textures[1] != NULL, st->alphaTest);
    fflush(f);
}

/* The software renderer works on triangle lists: indexed draws are unpacked into one. */
void gfxb_draw_indexed(const GfxDrawState *state, const GfxVertex *verts, int numVerts, const unsigned short *indices,
                       int numTriangles) {
    static GfxVertex sList[GFX_MAX_BATCH_TRIANGLES * 3];
    int i, count = numTriangles * 3;
    (void) numVerts;
    if (numTriangles > GFX_MAX_BATCH_TRIANGLES) {
        numTriangles = GFX_MAX_BATCH_TRIANGLES;
        count = numTriangles * 3;
    }
    for (i = 0; i < count; i++) {
        sList[i] = verts[indices[i]];
    }
    gfxb_draw_triangles(state, sList, numTriangles);
}

void gfxb_draw_triangles(const GfxDrawState *drawState, const GfxVertex *verts, int numTriangles) {
    static int sNullRender = -1;
    int t, i, n;
    GfxDrawState wideState;
    const GfxDrawState *state = drawState;
    if (sNullRender < 0) {
        sNullRender = getenv("DKR_NULL_RENDER") != NULL; // Skip rasterising, for profiling everything else.
    }
    if (sNullRender) {
        return;
    }
    log_combiner(state);
    if (sWide) {
        wideState = *drawState;
        wideState.scissor[0] = frame_x(drawState->scissor[0], drawState->flat);
        wideState.scissor[2] = frame_x(drawState->scissor[2], drawState->flat);
        state = &wideState;
    }
    for (t = 0; t < numTriangles; t++) {
        ClipVert in[3], clipped[8];
        ScreenVert s[8];
        for (i = 0; i < 3; i++) {
            to_clip(&in[i], &verts[t * 3 + i], gfx_flat_mode(state));
        }
        n = clip_near(in, 3, clipped);
        if (n < 3) {
            continue;
        }
        for (i = 0; i < n; i++) {
            if (clipped[i].w < 1e-6f) {
                clipped[i].w = 1e-6f;
            }
            project(&s[i], &clipped[i]);
        }
        for (i = 1; i + 1 < n; i++) {
            raster_triangle(state, &s[0], &s[i], &s[i + 1]);
        }
    }
}
