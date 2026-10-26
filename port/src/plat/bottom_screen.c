/*
 * The 3DS bottom screen, drawn in the game's own style (see bottom_screen.h for its areas). Everything that makes it
 * look like Diddy Kong Racing comes from the player's ROM, the same way the game loads it: the menu title font
 * (BigFont), the small font of its menus (SmallFont), the colourful font (FunFont), the title logo, the blue menu
 * arrows and the ON / OFF labels of the options menu. Until the ROM is loaded (and if a picture cannot be read),
 * text is drawn with the 8x8 font the platform passes in, so the screen works before then.
 *
 * The state (messages, menu page, buttons...) is changed by the bs_set_* calls; bs_take_changes copies it for
 * drawing, so the drawing can happen on another thread while the state changes again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "bottom_screen.h"
#include "plat.h"
#include "../../../include/asset_enums.h"

/* The ROM and the asset decompressor (os_pi.c, inflate.c). */
const unsigned char *port_rom_data(void);
unsigned long port_rom_size(void);
unsigned long port_assets_lut_start(void);
unsigned long port_assets_lut_end(void);
long port_inflate(const unsigned char *in, unsigned char *out);

/******************************** Pictures and fonts from the ROM ********************************/

typedef struct Picture {
    int width, height;
    unsigned char *rgba; /* 4 bytes a pixel, row by row. NULL: not loaded. */
} Picture;

typedef struct Glyph {
    unsigned char texture; /* 0xFF: none. */
    unsigned char advance, x, y, s, t, width, height;
} Glyph;

typedef struct Font {
    int loaded;
    int fixedAdvance, spaceWidth;
    Glyph glyphs[96];
    Picture textures[32];
} Font;

enum { FONT_SMALL, FONT_BIG, FONT_FUN, FONT_MONO };

static Font sFonts[3];
static Picture sLogo;
static Picture sArrowLeft, sArrowRight;
static Picture sOnOff[2][2]; /* [on, off][not chosen, chosen] */
static const unsigned char *sMono;

static unsigned long read_be32(const unsigned char *p) {
    return ((unsigned long) p[0] << 24) | ((unsigned long) p[1] << 16) | ((unsigned long) p[2] << 8) | p[3];
}

static unsigned read_be16(const unsigned char *p) {
    return (p[0] << 8) | p[1];
}

/* An asset section of the ROM, or NULL. */
static const unsigned char *section(int index, unsigned long *size) {
    const unsigned char *rom = port_rom_data();
    unsigned long start, end, base;
    if (rom == NULL) {
        return NULL;
    }
    base = port_assets_lut_end();
    start = read_be32(rom + port_assets_lut_start() + 4 + index * 4);
    end = read_be32(rom + port_assets_lut_start() + 8 + index * 4);
    if (end < start || base + end > port_rom_size()) {
        return NULL;
    }
    *size = end - start;
    return rom + base + start;
}

/* Decodes a 2D texture of the game (textures_sprites.c, load_texture) into RGBA. */
/*
 * Frame frame of a 2D texture of the game (textures_sprites.c, load_texture) into RGBA; the texture's place in its
 * sprite goes to *posX, *posY if they are not NULL.
 */
static int load_texture_frame(int id, int frame, Picture *out, int *posX, int *posY) {
    const unsigned char *table, *data, *header, *pixels;
    unsigned char *unpacked = NULL, *swapped = NULL;
    unsigned long tableSize, dataSize, start, end, need, stride;
    int width, height, format, bytes, i;

    table = section(ASSET_TEXTURES_2D_TABLE, &tableSize);
    data = section(ASSET_TEXTURES_2D, &dataSize);
    if (table == NULL || data == NULL || id < 0 || (unsigned long) (id + 2) * 4 > tableSize) {
        return 0;
    }
    start = read_be32(table + id * 4);
    end = read_be32(table + id * 4 + 4);
    if (end > dataSize || end < start + 0x20) {
        return 0;
    }
    data += start;
    width = data[0];
    height = data[1];
    format = data[2] & 0xF;
    // RGBA32, RGBA16, I8, IA16, IA8.
    bytes = format == 0 ? 4 : (format == 1 || format == 4) ? 2 : (format == 2 || format == 5) ? 1 : 0;
    // An animated texture's frames follow each other, each with its own header.
    stride = 0x20 + (unsigned long) width * height * bytes;
    need = stride * (frame + 1);
    if (bytes == 0 || width == 0 || height == 0 || frame < 0) {
        return 0;
    }
    if (data[0x1D]) {
        // Compressed: a 5 byte header (the size, little endian, then the level) and the deflate stream, which
        // unpacks to the header and the pixels.
        unsigned long size = data[0x20] | (data[0x21] << 8) | (data[0x22] << 16) | ((unsigned long) data[0x23] << 24);
        if (size < need || size > 256 * 1024) {
            return 0;
        }
        unpacked = malloc(size + 64);
        if (unpacked == NULL || port_inflate(data + 0x25, unpacked) < (long) need) {
            free(unpacked);
            return 0;
        }
        header = unpacked + stride * frame;
    } else {
        if (end - start < need) {
            return 0;
        }
        header = data + stride * frame;
    }
    pixels = header + 0x20;
    if (read_be16(header + 6) & 0x400) {
        // Interlaced (for the N64's texture memory): on every other row, each pair of 8 byte (4 for 16 bits and less)
        // blocks is swapped.
        int block = bytes == 4 ? 8 : 4, rowBytes = width * bytes, y, x, j;
        swapped = malloc(stride);
        if (swapped == NULL) {
            free(unpacked);
            return 0;
        }
        memcpy(swapped, pixels, (unsigned long) width * height * bytes);
        for (y = 1; y < height; y += 2) {
            unsigned char *row = swapped + y * rowBytes;
            for (x = 0; x + block * 2 <= rowBytes; x += block * 2) {
                for (j = 0; j < block; j++) {
                    unsigned char t = row[x + j];
                    row[x + j] = row[x + block + j];
                    row[x + block + j] = t;
                }
            }
        }
        pixels = swapped;
    }
    if (posX != NULL) {
        *posX = (signed char) header[3];
        *posY = (signed char) header[4];
    }
    out->rgba = malloc(width * height * 4);
    if (out->rgba == NULL) {
        free(unpacked);
        free(swapped);
        return 0;
    }
    out->width = width;
    out->height = height;
    for (i = 0; i < width * height; i++) {
        unsigned char *o = out->rgba + i * 4;
        unsigned v;
        switch (format) {
            case 0:
                memcpy(o, pixels + i * 4, 4);
                break;
            case 1:
                v = read_be16(pixels + i * 2);
                o[0] = ((v >> 11) & 31) * 255 / 31;
                o[1] = ((v >> 6) & 31) * 255 / 31;
                o[2] = ((v >> 1) & 31) * 255 / 31;
                o[3] = (v & 1) ? 255 : 0;
                break;
            case 2:
                o[0] = o[1] = o[2] = o[3] = pixels[i];
                break;
            case 4:
                o[0] = o[1] = o[2] = pixels[i * 2];
                o[3] = pixels[i * 2 + 1];
                break;
            default:
                // IA8: 4 bits of intensity, 4 of alpha.
                o[0] = o[1] = o[2] = (pixels[i] >> 4) * 17;
                o[3] = (pixels[i] & 15) * 17;
                break;
        }
    }
    free(unpacked);
    free(swapped);
    return 1;
}

static int load_texture(int id, Picture *out) {
    return load_texture_frame(id, 0, out, NULL, NULL);
}

/* A font of the game (font.c, load_fonts and load_font). */
static int load_font(int index, Font *font) {
    unsigned long size;
    const unsigned char *fonts = section(ASSET_FONTS, &size), *f;
    int i;

    if (fonts == NULL || size < 4 || index >= (int) read_be32(fonts) || 4 + (index + 1) * 0x400UL > size) {
        return 0;
    }
    f = fonts + 4 + index * 0x400;
    font->fixedAdvance = read_be16(f + 0x20);
    font->spaceWidth = read_be16(f + 0x24);
    for (i = 0; i < 32; i++) {
        int id = (short) read_be16(f + 0x40 + i * 2);
        if (id == -1) {
            break;
        }
        if (!load_texture(id, &font->textures[i])) {
            return 0;
        }
    }
    for (i = 0; i < 96; i++) {
        const unsigned char *c = f + 0x100 + i * 8;
        Glyph *g = &font->glyphs[i];
        g->texture = c[0];
        g->advance = c[1];
        g->x = c[2];
        g->y = c[3];
        g->s = c[4];
        g->t = c[5];
        g->width = c[6];
        g->height = c[7];
        if (g->texture != 0xFF && (g->texture >= 32 || font->textures[g->texture].rgba == NULL ||
                                   g->s + g->width > font->textures[g->texture].width ||
                                   g->t + g->height > font->textures[g->texture].height)) {
            g->texture = 0xFF;
        }
    }
    font->loaded = 1;
    return 1;
}

/* Halves a picture, averaging each 2x2 block (the menu arrows are drawn at half size). */
static void halve(Picture *p) {
    int w = p->width / 2, h = p->height / 2, x, y, c;
    unsigned char *small;
    if (p->rgba == NULL || (small = malloc(w * h * 4)) == NULL) {
        return;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const unsigned char *a = p->rgba + ((y * 2) * p->width + x * 2) * 4, *b = a + p->width * 4;
            int alpha = a[3] + a[7] + b[3] + b[7];
            for (c = 0; c < 3; c++) {
                // Weighted by alpha, so transparent texels do not darken the edges.
                int sum = a[c] * a[3] + a[4 + c] * a[7] + b[c] * b[3] + b[4 + c] * b[7];
                small[(y * w + x) * 4 + c] = alpha ? sum / alpha : 0;
            }
            small[(y * w + x) * 4 + 3] = alpha / 4;
        }
    }
    free(p->rgba);
    p->rgba = small;
    p->width = w;
    p->height = h;
}

/* The title logo is ten strips of 16x64. */
static void load_logo(void) {
    Picture strip;
    int i, y;
    sLogo.rgba = calloc(160 * 64, 4);
    if (sLogo.rgba == NULL) {
        return;
    }
    sLogo.width = 160;
    sLogo.height = 64;
    for (i = 0; i < 10; i++) {
        if (!load_texture(ASSET_TEX2D_MENU_TITLE_0 + i, &strip) || strip.width != 16 || strip.height != 64) {
            free(sLogo.rgba);
            sLogo.rgba = NULL;
            return;
        }
        for (y = 0; y < 64; y++) {
            memcpy(sLogo.rgba + (y * 160 + i * 16) * 4, strip.rgba + y * 16 * 4, 16 * 4);
        }
        free(strip.rgba);
    }
}

static unsigned sChanges;
static void load_race_art(void);

void bs_load_assets(void) {
    static const int sFontAssets[3] = { ASSET_FONTS_SMALLFONT, ASSET_FONTS_BIGFONT, ASSET_FONTS_FUNFONT };
    int i;
    for (i = 0; i < 3; i++) {
        if (!load_font(sFontAssets[i], &sFonts[i])) {
            plat_log("bottom screen: font %d not loaded", sFontAssets[i]);
        }
    }
    load_logo();
    if (load_texture(ASSET_TEX2D_MENU_ARROWBLUELEFT, &sArrowLeft)) {
        halve(&sArrowLeft);
    }
    if (load_texture(ASSET_TEX2D_MENU_ARROWBLUERIGHT, &sArrowRight)) {
        halve(&sArrowRight);
    }
    load_texture(ASSET_TEX2D_MENU_LABELONOPTIONNONSELECTED, &sOnOff[0][0]);
    load_texture(ASSET_TEX2D_MENU_LABELONOPTIONSELECTED, &sOnOff[0][1]);
    load_texture(ASSET_TEX2D_MENU_LABELOFFOPTIONNONSELECTED, &sOnOff[1][0]);
    load_texture(ASSET_TEX2D_MENU_LABELOFFOPTIONSELECTED, &sOnOff[1][1]);
    load_race_art();
    sChanges = (1 << BS_AREA_COUNT) - 1;
}

/******************************** Drawing ********************************/

typedef struct Colour {
    unsigned char r, g, b, a;
} Colour;

static const Colour WHITE = { 255, 255, 255, 255 };
static const Colour YELLOW = { 255, 226, 60, 255 };
static const Colour HINT = { 150, 200, 255, 255 };
static const Colour DIM = { 185, 195, 225, 255 };
static const Colour BOX_FILL = { 0, 8, 40, 120 };
static const Colour BOX_EDGE = { 140, 180, 255, 150 };

static unsigned short *sImage;
static int sClipTop, sClipEnd; /* The rows of the area being drawn. */
static unsigned short sBackground[BS_WIDTH * BS_HEIGHT];
static int sBackgroundMade;

static unsigned short rgb565(int r, int g, int b) {
    return (unsigned short) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void plot(int x, int y, int r, int g, int b, int a) {
    unsigned short *p, d;
    int dr, dg, db;
    if (x < 0 || x >= BS_WIDTH || y < sClipTop || y >= sClipEnd || a <= 0) {
        return;
    }
    p = &sImage[y * BS_WIDTH + x];
    if (a >= 255) {
        *p = rgb565(r, g, b);
        return;
    }
    d = *p;
    dr = (d >> 11) << 3;
    dg = ((d >> 5) & 63) << 2;
    db = (d & 31) << 3;
    *p = rgb565(dr + (r - dr) * a / 255, dg + (g - dg) * a / 255, db + (b - db) * a / 255);
}

static void fill(int x0, int y0, int x1, int y1, Colour c) {
    int x, y;
    for (y = y0 < sClipTop ? sClipTop : y0; y < y1 && y < sClipEnd; y++) {
        for (x = x0; x < x1; x++) {
            plot(x, y, c.r, c.g, c.b, c.a);
        }
    }
}

/* A box with rounded corners and a one pixel edge, like the game's dialogue boxes. */
static void box(int x0, int y0, int x1, int y1, Colour inside, Colour edge) {
    int y;
    fill(x0 + 1, y0 + 1, x1 - 1, y1 - 1, inside);
    fill(x0 + 2, y0, x1 - 2, y0 + 1, edge);
    fill(x0 + 2, y1 - 1, x1 - 2, y1, edge);
    for (y = y0 + 2; y < y1 - 2; y++) {
        plot(x0, y, edge.r, edge.g, edge.b, edge.a);
        plot(x1 - 1, y, edge.r, edge.g, edge.b, edge.a);
    }
    plot(x0 + 1, y0 + 1, edge.r, edge.g, edge.b, edge.a);
    plot(x1 - 2, y0 + 1, edge.r, edge.g, edge.b, edge.a);
    plot(x0 + 1, y1 - 2, edge.r, edge.g, edge.b, edge.a);
    plot(x1 - 2, y1 - 2, edge.r, edge.g, edge.b, edge.a);
}

/* Part of a picture, tinted (255 each: as it is); a black tint with alpha draws its shadow. */
static void picture_part(const Picture *p, int sx, int sy, int w, int h, int x, int y, Colour tint) {
    int i, j;
    if (p->rgba == NULL) {
        return;
    }
    for (j = 0; j < h; j++) {
        if (y + j < sClipTop || y + j >= sClipEnd) {
            continue;
        }
        for (i = 0; i < w; i++) {
            const unsigned char *t = p->rgba + ((sy + j) * p->width + sx + i) * 4;
            if (t[3] != 0) {
                plot(x + i, y + j, t[0] * tint.r / 255, t[1] * tint.g / 255, t[2] * tint.b / 255, t[3] * tint.a / 255);
            }
        }
    }
}

static void picture(const Picture *p, int x, int y) {
    picture_part(p, 0, 0, p->width, p->height, x, y, WHITE);
}

static int mono_char(int x, int y, unsigned char c, Colour colour) {
    int i, j;
    if (sMono != NULL) {
        for (j = 0; j < 8; j++) {
            unsigned char bits = sMono[c * 8 + j];
            for (i = 0; i < 8; i++) {
                if (bits & (0x80 >> i)) {
                    plot(x + i, y + j, colour.r, colour.g, colour.b, colour.a);
                }
            }
        }
    }
    return 8;
}

static int font_ready(int font) {
    return font != FONT_MONO && sFonts[font].loaded;
}

/* How far a character moves the pen (font.c, get_text_width). */
static int advance(const Font *f, unsigned char c) {
    if (c <= 0x20 || c >= 0x80) {
        return f->spaceWidth;
    }
    if (f->glyphs[c - 0x20].texture == 0xFF) {
        return 0;
    }
    return f->fixedAdvance ? f->fixedAdvance : f->glyphs[c - 0x20].advance;
}

static int text_width(int font, const char *text) {
    int width = 0;
    if (!font_ready(font)) {
        return (int) strlen(text) * 8;
    }
    for (; *text != '\0'; text++) {
        width += advance(&sFonts[font], (unsigned char) *text);
    }
    return width;
}

/* Draws text from x (until right, if text would go further) with its top at y. Returns where it ended. */
static int text_clipped(int font, int x, int y, const char *text, Colour colour, int shadow, int right) {
    const Font *f = &sFonts[font];
    if (!font_ready(font)) {
        // Before the ROM's fonts are loaded: the 8x8 font, a little lower so it sits like the small font.
        for (; *text != '\0' && x + 8 <= right; text++) {
            if (shadow) {
                mono_char(x + 1, y + 2, (unsigned char) *text, (Colour) { 0, 0, 0, 160 });
            }
            x += mono_char(x, y + 1, (unsigned char) *text, colour);
        }
        return x;
    }
    for (; *text != '\0'; text++) {
        unsigned char c = (unsigned char) *text;
        int step = advance(f, c);
        if (x + step > right) {
            break;
        }
        if (c > 0x20 && c < 0x80 && f->glyphs[c - 0x20].texture != 0xFF) {
            const Glyph *g = &f->glyphs[c - 0x20];
            const Picture *t = &f->textures[g->texture];
            if (shadow) {
                picture_part(t, g->s, g->t, g->width, g->height, x + g->x + 1, y + g->y + 1, (Colour) { 0, 0, 0, 170 });
            }
            picture_part(t, g->s, g->t, g->width, g->height, x + g->x, y + g->y, colour);
        }
        x += step;
    }
    return x;
}

static int text(int font, int x, int y, const char *s, Colour colour, int shadow) {
    return text_clipped(font, x, y, s, colour, shadow, BS_WIDTH);
}

static void text_centred(int font, int x0, int x1, int y, const char *s, Colour colour, int shadow) {
    int width = text_width(font, s);
    text_clipped(font, x0 + (x1 - x0 - width) / 2, y, s, colour, shadow, x1);
}

/* Dense text (the profiler and the watchdog) in 8 pixel columns: the 8x8 font, or the small font centred. */
static void mono_text(int x, int y, const char *s, Colour colour) {
    for (; *s != '\0' && x + 8 <= BS_WIDTH; s++, x += 8) {
        if (sMono != NULL || !font_ready(FONT_SMALL)) {
            mono_char(x, y, (unsigned char) *s, colour);
        } else {
            char one[2] = { *s, '\0' };
            text_centred(FONT_SMALL, x, x + 8, y - 2, one, colour, 0);
        }
    }
}

/*
 * The background: deep blue, lighter at the top, with a faint chequered flag pattern; the status lines sit on a
 * darker strip.
 */
static void make_background(void) {
    int x, y;
    for (y = 0; y < BS_HEIGHT; y++) {
        int r = 26 - y * 18 / BS_HEIGHT, g = 58 - y * 40 / BS_HEIGHT, b = 160 - y * 92 / BS_HEIGHT;
        if (y >= BS_STATUS_Y) {
            r = 4;
            g = 8;
            b = 34;
        }
        for (x = 0; x < BS_WIDTH; x++) {
            int light = (y < BS_STATUS_Y && ((x / 20) + (y / 20)) % 2 == 0) ? 7 : 0;
            sBackground[y * BS_WIDTH + x] = rgb565(r + light, g + light, b + light);
        }
    }
    // A thin light line above the status strip.
    for (x = 0; x < BS_WIDTH; x++) {
        sBackground[(BS_STATUS_Y - 1) * BS_WIDTH + x] = rgb565(60, 90, 170);
    }
    sBackgroundMade = 1;
}

/******************************** State ********************************/

#define MESSAGES 24
#define OVERLAY_LINES 12

typedef struct State {
    char messages[MESSAGES][160];
    int messageCount; /* The newest is messages[(messageCount - 1) % MESSAGES]. */
    char overlay[OVERLAY_LINES][48];
    int overlayLines;
    int menuShown;
    BsMenu menu;
    int buttonCount; /* 0: the panel text instead. */
    char buttons[4][40];
    char panelText[64];
    char status[2][48];
    int raceShown;
    RaceHud race;
} State;

static State sState, sDrawn;

void bs_init(const unsigned char *monoFont) {
    sMono = monoFont;
    if (!sBackgroundMade) {
        make_background();
    }
    sChanges = (1 << BS_AREA_COUNT) - 1;
}

void bs_add_message(const char *text) {
    snprintf(sState.messages[sState.messageCount % MESSAGES], sizeof(sState.messages[0]), "%s", text);
    sState.messageCount++;
    sChanges |= 1 << BS_AREA_TOP;
}

void bs_clear_messages(void) {
    sState.messageCount = 0;
    sChanges |= 1 << BS_AREA_TOP;
}

void bs_set_overlay(const char *text) {
    int line = 0;
    while (line < OVERLAY_LINES && *text != '\0') {
        const char *end = strchr(text, '\n');
        int length = end != NULL ? (int) (end - text) : (int) strlen(text);
        snprintf(sState.overlay[line++], sizeof(sState.overlay[0]), "%.*s", length, text);
        text += length + (end != NULL);
    }
    sState.overlayLines = line;
    sChanges |= 1 << BS_AREA_TOP;
}

void bs_show_menu(const BsMenu *menu) {
    sState.menuShown = menu != NULL;
    if (menu != NULL) {
        sState.menu = *menu;
    }
    sChanges |= 1 << BS_AREA_TOP;
}

void bs_set_buttons(const char *const *labels, int count) {
    int i;
    sState.buttonCount = count > 4 ? 4 : count;
    for (i = 0; i < sState.buttonCount; i++) {
        snprintf(sState.buttons[i], sizeof(sState.buttons[0]), "%s", labels[i]);
    }
    sChanges |= 1 << BS_AREA_BUTTONS;
}

void bs_set_panel_text(const char *text) {
    sState.buttonCount = 0;
    snprintf(sState.panelText, sizeof(sState.panelText), "%s", text);
    sChanges |= 1 << BS_AREA_BUTTONS;
}

void bs_set_status_line(int line, const char *text) {
    if (line >= 0 && line < 2 && strcmp(sState.status[line], text) != 0) {
        snprintf(sState.status[line], sizeof(sState.status[0]), "%s", text);
        sChanges |= 1 << BS_AREA_STATUS;
    }
}

void bs_set_race_hud(const RaceHud *hud) {
    sState.raceShown = hud != NULL;
    if (hud != NULL) {
        sState.race = *hud;
    }
    sChanges |= 1 << BS_AREA_TOP;
}

unsigned bs_take_changes(void) {
    unsigned changes = sChanges;
    sChanges = 0;
    if (changes != 0) {
        sDrawn = sState;
    }
    return changes;
}

/******************************** The areas ********************************/

static const char *newest_message(const State *s) {
    return s->messageCount > 0 ? s->messages[(s->messageCount - 1) % MESSAGES] : "";
}

/*
 * The messages box: the newest messages, wrapped to the box's width, as many as fit, the newest at the bottom.
 */
static void draw_messages(const State *s, int y0, int y1) {
    enum { MAX_LINES = 24 };
    const char *starts[MAX_LINES];
    int lengths[MAX_LINES], newest[MAX_LINES];
    int font = font_ready(FONT_SMALL) ? FONT_SMALL : FONT_MONO, lineHeight = font == FONT_SMALL ? 12 : 9;
    int fit = (y1 - y0 - 6) / lineHeight, count = 0, m, i;
    int width = BS_WIDTH - 24;

    box(4, y0, BS_WIDTH - 4, y1, BOX_FILL, BOX_EDGE);
    // From the newest message back, until the box is full; each message's lines are found first to last.
    for (m = s->messageCount - 1; m >= 0 && m >= s->messageCount - MESSAGES && count < fit; m--) {
        const char *text = s->messages[m % MESSAGES];
        const char *lineStarts[8];
        int lineLengths[8], lines = 0;
        while (*text == ' ') {
            text++;
        }
        do {
            int length = (int) strlen(text), cut = length;
            char part[160];
            snprintf(part, sizeof(part), "%s", text);
            while (cut > 0 && text_width(font, part) > width) {
                // Back to the space before the last word that does not fit (or cut the word).
                int space = cut - 1;
                while (space > 0 && text[space] != ' ') {
                    space--;
                }
                cut = space > 0 ? space : cut - 1;
                part[cut] = '\0';
            }
            if (cut == 0) {
                cut = length;
            }
            if (lines < 8) {
                lineStarts[lines] = text;
                lineLengths[lines] = cut;
                lines++;
            }
            text += cut;
            while (*text == ' ') {
                text++;
            }
        } while (*text != '\0');
        for (i = lines - 1; i >= 0 && count < fit; i--) {
            starts[count] = lineStarts[i];
            lengths[count] = lineLengths[i];
            newest[count] = m == s->messageCount - 1;
            count++;
        }
    }
    for (i = 0; i < count; i++) {
        char line[160];
        int y = y0 + 3 + (count - 1 - i) * lineHeight;
        snprintf(line, sizeof(line), "%.*s", lengths[i], starts[i]);
        text(font, 12, y, line, newest[i] ? WHITE : DIM, 1);
    }
}

/* The controls, in two columns. */
static void draw_controls(int y0, int y1) {
    static const char *const sControls[10][2] = {
        { "Circle Pad", "Steer" },   { "X / Y", "C-Up / C-Left" },
        { "A", "Accelerate" },       { "C-Stick", "C buttons" },
        { "B", "Brake / reverse" },  { "SELECT", "N64 L" },
        { "L / ZL", "Use item" },    { "START", "Pause" },
        { "R / ZR", "Hop / drift" }, { "SELECT+touch", "Bug report" },
    };
    int i, lineHeight = font_ready(FONT_SMALL) ? 12 : 10, keyWidth[2] = { 0, 0 };
    box(4, y0, BS_WIDTH - 4, y1, BOX_FILL, BOX_EDGE);
    for (i = 0; i < 10; i++) {
        int width = text_width(FONT_SMALL, sControls[i][0]) + 8;
        if (width > keyWidth[i % 2]) {
            keyWidth[i % 2] = width;
        }
    }
    for (i = 0; i < 10; i++) {
        int x = (i % 2) ? 164 : 12, y = y0 + 4 + (i / 2) * lineHeight;
        if (!font_ready(FONT_SMALL) && (i % 2)) {
            continue; // The 8x8 font only fits one column; the second waits for the ROM.
        }
        text(FONT_SMALL, x, y, sControls[i][0], YELLOW, 1);
        text(FONT_SMALL, x + keyWidth[i % 2], y, sControls[i][1], WHITE, 1);
    }
}

static void draw_logo(void) {
    if (sLogo.rgba != NULL) {
        picture(&sLogo, (BS_WIDTH - 160) / 2, 3);
    } else {
        text_centred(FONT_MONO, 0, BS_WIDTH, 20, "Diddy Kong Racing", YELLOW, 1);
        text_centred(FONT_MONO, 0, BS_WIDTH, 36, "Nintendo 3DS port", DIM, 1);
    }
}

static void draw_overlay(const State *s, int y0, int y1) {
    int i;
    box(2, y0, BS_WIDTH - 2, y1, (Colour) { 0, 0, 20, 170 }, BOX_EDGE);
    for (i = 0; i < s->overlayLines; i++) {
        mono_text(4, y0 + 2 + i * 8, s->overlay[i], i == 0 ? YELLOW : WHITE);
    }
}

static void draw_home(const State *s) {
    draw_logo();
    if (s->overlayLines > 0) {
        draw_messages(s, 70, 98);
        draw_overlay(s, 100, BS_BUTTONS_Y);
    } else {
        draw_controls(70, 136);
        draw_messages(s, 140, BS_BUTTONS_Y - 1);
    }
}

/*
 * A value of "On" or "Off" is shown as a switch with the game's labels: ON lit, or OFF dim (both side by side, the
 * chosen one lit, read the wrong way round: OFF looked brighter when off).
 */
static int draw_on_off(const char *value, int x, int y) {
    int on = strcmp(value, "On") == 0;
    // Of the game's two versions of each label, the one with white letters is the brighter one; OFF gets the other,
    // greyed.
    const Picture *label = on ? &sOnOff[0][0] : &sOnOff[1][1];
    if ((!on && strcmp(value, "Off") != 0) || label->rgba == NULL) {
        return 0;
    }
    picture_part(label, 0, 0, label->width, label->height, x, y, on ? WHITE : (Colour) { 150, 150, 160, 255 });
    return 1;
}

static void arrow(int left, int x, int y) {
    const Picture *p = left ? &sArrowLeft : &sArrowRight;
    if (p->rgba != NULL) {
        picture(p, x, y);
    } else {
        text(FONT_MONO, x, y + 1, left ? "<" : ">", YELLOW, 1);
    }
}

static void draw_menu(const State *s) {
    const BsMenu *m = &s->menu;
    int i;

    if (font_ready(FONT_BIG)) {
        // The game's menu titles are capitals of the big font.
        char title[32];
        for (i = 0; m->title[i] != '\0' && i < (int) sizeof(title) - 1; i++) {
            title[i] = (m->title[i] >= 'a' && m->title[i] <= 'z') ? m->title[i] - 32 : m->title[i];
        }
        title[i] = '\0';
        text_centred(FONT_BIG, 0, BS_WIDTH, 2, title, WHITE, 0);
    } else {
        text_centred(FONT_MONO, 0, BS_WIDTH, 10, m->title, YELLOW, 1);
    }
    if (m->pages[0] != '\0') {
        text(FONT_SMALL, BS_WIDTH - 8 - text_width(FONT_SMALL, m->pages), 12, m->pages, HINT, 1);
    }
    for (i = 0; i < m->rowCount && i < BS_MENU_ROWS; i++) {
        const BsMenuRow *row = &m->rows[i];
        int y = BS_MENU_FIRST_Y + i * BS_MENU_ROW_HEIGHT;
        int onOff = strcmp(row->value, "On") == 0 || strcmp(row->value, "Off") == 0;
        box(4, y + 1, BS_WIDTH - 4, y + BS_MENU_ROW_HEIGHT - 1, BOX_FILL, BOX_EDGE);
        // The label, with its hint under it, or alone in the middle of the row.
        text_clipped(FONT_SMALL, 12, row->hint[0] != '\0' ? y + 2 : y + 7, row->label, WHITE, 1, BS_MENU_VALUE_X);
        text_clipped(FONT_SMALL, 12, y + 12, row->hint, HINT, 1,
                     row->value[0] == '\0' ? BS_WIDTH - 24 : onOff ? BS_WIDTH - 52 : BS_MENU_VALUE_X + 4);
        if (row->value[0] == '\0') {
            arrow(0, BS_WIDTH - 22, y + 6);
        } else if (!draw_on_off(row->value, BS_WIDTH - 12 - 36, y + 3)) {
            arrow(1, BS_MENU_VALUE_X + 6, y + 6);
            arrow(0, BS_WIDTH - 22, y + 6);
            text_centred(FONT_SMALL, BS_MENU_VALUE_X + 16, BS_WIDTH - 24, y + 7, row->value, YELLOW, 1);
        }
    }
    text_clipped(FONT_SMALL, 8, 177, m->note, DIM, 1, BS_WIDTH - 8);
    text_clipped(FONT_SMALL, 8, 188, newest_message(s), YELLOW, 1, BS_WIDTH - 8);
}

/* Buttons in the style of the game's blue arrows: blue, edged in yellow; grey if they can't be tapped, green if on. */
static void draw_buttons(const State *s) {
    int i, count = s->buttonCount, cell = count > 1 ? 40 / count : 40;

    if (count == 0) {
        box(4, BS_BUTTONS_Y + 2, BS_WIDTH - 4, BS_STATUS_Y - 3, BOX_FILL, BOX_EDGE);
        text_centred(FONT_SMALL, 8, BS_WIDTH - 8, BS_BUTTONS_Y + 6, s->panelText, YELLOW, 1);
        return;
    }
    for (i = 0; i < count; i++) {
        const char *label = s->buttons[i];
        int grey = label[0] == '-', on = label[0] == '+', x0 = (i * cell + 1) * 8, x1 = (i + 1) * cell * 8, y, row;
        Colour top = { 70, 130, 255, 255 }, bottom = { 20, 50, 170, 255 }, edge = { 255, 214, 40, 255 };
        if (count == 1 || x1 > BS_WIDTH - 8) {
            x1 = BS_WIDTH - 8;
        }
        if (grey && label[1] == '\0') {
            continue; // A gap.
        }
        if (grey) {
            top = (Colour) { 150, 150, 160, 255 };
            bottom = (Colour) { 90, 90, 104, 255 };
            edge = (Colour) { 190, 190, 200, 255 };
        } else if (on) {
            top = (Colour) { 90, 220, 90, 255 };
            bottom = (Colour) { 20, 130, 30, 255 };
        }
        // A dark outline, the yellow edge, then the fill getting darker downwards.
        box(x0 - 1, BS_BUTTONS_Y + 1, x1 + 1, BS_STATUS_Y - 2, (Colour) { 0, 0, 0, 0 }, (Colour) { 0, 0, 30, 220 });
        box(x0, BS_BUTTONS_Y + 2, x1, BS_STATUS_Y - 3, (Colour) { 0, 0, 0, 0 }, edge);
        for (y = BS_BUTTONS_Y + 3, row = 0; y < BS_STATUS_Y - 4; y++, row++) {
            int span = BS_STATUS_Y - 4 - (BS_BUTTONS_Y + 3);
            Colour c = { top.r + (bottom.r - top.r) * row / span, top.g + (bottom.g - top.g) * row / span,
                         top.b + (bottom.b - top.b) * row / span, 255 };
            fill(x0 + 1, y, x1 - 1, y + 1, c);
        }
        // A highlight along the top.
        fill(x0 + 2, BS_BUTTONS_Y + 3, x1 - 2, BS_BUTTONS_Y + 4, (Colour) { 255, 255, 255, 90 });
        if (font_ready(FONT_FUN)) {
            char upper[40];
            int j;
            const char *t = label + (grey || on);
            for (j = 0; t[j] != '\0' && j < (int) sizeof(upper) - 1; j++) {
                upper[j] = (t[j] >= 'a' && t[j] <= 'z') ? t[j] - 32 : t[j];
            }
            upper[j] = '\0';
            text_centred(FONT_FUN, x0, x1, BS_BUTTONS_Y + 6, upper, grey ? (Colour) { 120, 120, 130, 255 } : WHITE,
                         !grey);
        } else {
            text_centred(FONT_SMALL, x0, x1, BS_BUTTONS_Y + 6, label + (grey || on), grey ? DIM : WHITE, !grey);
        }
    }
}

static void draw_status(const State *s) {
    static const Colour sStatusColour = { 120, 140, 200, 255 };
    mono_text(4, BS_STATUS_Y, s->status[0], sStatusColour);
    mono_text(4, BS_STATUS_Y + 8, s->status[1], sStatusColour);
}

/******************************** The race HUD ********************************/

/*
 * During a race (race_hud.h) the top area shows the race in the layout chosen from the mockups ("standings tower"):
 *   x 4-92     the racers in race order, each as the game's portrait with the time gap to the player
 *   x 96-186   the item held, then the bananas and the last and best lap times
 *   x 190-316  the position, lap and race time, then the track's map with every racer on it
 * Everything is the game's own art at its own size, so nothing is blurred. The panels and the map,
 * which only change with the track, are drawn once into sRaceBase; each update draws the rest over a copy of it.
 */
#define RACE_ROWS_Y 200
#define TOWER_X0 4
#define TOWER_X1 92
#define MIDDLE_X0 96
#define MIDDLE_X1 186
#define RIGHT_X0 190
#define RIGHT_X1 316
#define UPPER_Y0 4
#define UPPER_Y1 92
#define LOWER_Y0 96
#define LOWER_Y1 198
#define TOWER_ROW_Y 6
#define TOWER_ROW_HEIGHT 24

/* The race art from the ROM, loaded with the fonts. */
static Picture sPortraits[10]; /* By the game's Character, halved to 20x20. */
static Picture sItems[15];     /* balloon_type * 3 + balloon_level. */
static Picture sItemCounts[10];
static Picture sPlaces[8], sSuffixes[4];
static Picture sDigits[10], sSmallDigits[10], sColon, sSmallColon, sSlash;
static Picture sLapLabel, sTimeLabel, sFinalLabel, sLapSmallLabel, sFinishLabel, sBanana, sTimes;

/* The minimap's colour of each character (game_ui.c, gHudMinimapColours). */
static const Colour sCharacterColours[10] = {
    { 255, 160, 0, 255 }, { 255, 255, 0, 255 },   { 0, 128, 255, 255 }, { 255, 255, 255, 255 }, { 0, 175, 0, 255 },
    { 0, 255, 128, 255 }, { 255, 0, 0, 255 },     { 255, 128, 190, 255 }, { 128, 128, 128, 255 }, { 0, 0, 255, 255 },
};

static const Colour GOLD = { 255, 214, 40, 255 };
static const Colour AHEAD = { 255, 150, 140, 255 };
static const Colour BEHIND = { 140, 255, 160, 255 };

/* The track's map, drawn smooth at the size its panel allows, with its offsets, for the sprite in sMapSprite. */
static Picture sMap;
static int sMapSprite = -2, sMapMirrored, sMapX, sMapY, sMapTexX, sMapTexY, sMapTexWidth;
static unsigned char sMapColour[3];
static float sMapScale;
static unsigned short sRaceBase[BS_WIDTH * RACE_ROWS_Y];
static int sRaceBaseMade;

/*
 * The banana counter's sprite is one picture in two pieces (the stem and upper half, then the lower curve), each
 * placed by its offset in the sprite.
 */
static void load_banana(void) {
    Picture part[2];
    int x[2], y[2], i, x0, y0, x1, y1, px, py;
    for (i = 0; i < 2; i++) {
        if (!load_texture_frame(ASSET_TEX2D_HUD_BANANAICON_0 + i, 0, &part[i], &x[i], &y[i])) {
            if (i == 1) {
                free(part[0].rgba);
            }
            return;
        }
    }
    x0 = x[0] < x[1] ? x[0] : x[1];
    y0 = y[0] < y[1] ? y[0] : y[1];
    x1 = x[0] + part[0].width > x[1] + part[1].width ? x[0] + part[0].width : x[1] + part[1].width;
    y1 = y[0] + part[0].height > y[1] + part[1].height ? y[0] + part[0].height : y[1] + part[1].height;
    sBanana.width = x1 - x0;
    sBanana.height = y1 - y0;
    sBanana.rgba = calloc((unsigned long) sBanana.width * sBanana.height, 4);
    for (i = 0; i < 2; i++) {
        for (py = 0; sBanana.rgba != NULL && py < part[i].height; py++) {
            for (px = 0; px < part[i].width; px++) {
                const unsigned char *t = part[i].rgba + (py * part[i].width + px) * 4;
                if (t[3] != 0) {
                    memcpy(sBanana.rgba + ((y[i] - y0 + py) * sBanana.width + x[i] - x0 + px) * 4, t, 4);
                }
            }
        }
        free(part[i].rgba);
    }
}

/*
 * The ROM's colons are two short bars a single dark row apart (the small one's are 2x1 pixels), which read as "=" on
 * this screen. These are two dots shaded like the digits: each row takes the colours of the same row of the digit 1
 * (its lit edge, middle and shaded edge), with the digits' dark edge and the two dark rows under them. top is the
 * first lit row of each dot, so the pair sits around the middle of the digits.
 */
static void load_colon(const Picture *digit, Picture *out, int dot, int top0, int top1) {
    int x, y, d, size = dot + 2;
    if (digit->rgba == NULL) {
        return;
    }
    out->width = size;
    out->height = digit->height;
    out->rgba = calloc((unsigned long) out->width * out->height, 4);
    for (d = 0; out->rgba != NULL && d < 2; d++) {
        int top = d ? top1 : top0;
        for (y = top - 1; y <= top + dot + 1 && y < out->height; y++) {
            int first = -1, last = -1, i;
            const unsigned char *row = digit->rgba + y * digit->width * 4;
            // The lit run of the digit's row.
            for (i = 0; i < digit->width; i++) {
                if (row[i * 4 + 3] != 0 && row[i * 4] + row[i * 4 + 1] + row[i * 4 + 2] > 120) {
                    if (first < 0) {
                        first = i;
                    }
                    last = i;
                }
            }
            for (x = 0; x < size; x++) {
                unsigned char *o = out->rgba + (y * out->width + x) * 4;
                int edgeX = x == 0 || x == size - 1, edgeY = y == top - 1 || y >= top + dot;
                if (edgeX && edgeY) {
                    // Rounded corners, like the digits'.
                    continue;
                }
                if (edgeX || edgeY || first < 0) {
                    o[0] = o[1] = o[2] = 0;
                } else {
                    int at = x == 1 ? first : x == dot ? last : (first + last) / 2;
                    memcpy(o, row + at * 4, 3);
                }
                o[3] = 255;
            }
        }
    }
}


/*
 * The bosses' and Taj's portraits. The ROM has none, and drawn ones cannot be shipped, so they are the player's own:
 * 40x40 PNGs like the game's portraits (frame included), in sdmc:/3ds/dkr/portraits (port/tools/portraits makes
 * them). A file that is missing, is not a PNG or is any other size is not used, and that racer gets a name box.
 */
#define PORTRAIT_SIZE 40
#define PORTRAIT_FILE_MAX (256 * 1024)
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS PORTRAIT_SIZE
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "third_party/stb_image.h"
#pragma GCC diagnostic pop

static const char *const sSpecialNames[RACE_HUD_SPECIALS] = { "tricky", "bluey", "bubbler", "smokey", "wizpig", "taj" };
static Picture sSpecialPortraits[RACE_HUD_SPECIALS]; /* Halved to 20x20; rgba NULL: the name box. */

static void load_special_portrait(int index, Picture *out) {
    char path[256];
    unsigned char *file, *rgba;
    long size;
    int w, h, channels;
    FILE *f;
    snprintf(path, sizeof(path), "%s/portraits/%s.png", plat_data_dir(), sSpecialNames[index]);
    if ((f = fopen(path, "rb")) == NULL) {
        return;
    }
    file = NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 && size <= PORTRAIT_FILE_MAX && fseek(f, 0, SEEK_SET) == 0 &&
        (file = malloc(size)) != NULL && fread(file, 1, size, f) == (size_t) size) {
        // The size is checked from the header before anything is decoded.
        if (!stbi_info_from_memory(file, (int) size, &w, &h, &channels) || w != PORTRAIT_SIZE || h != PORTRAIT_SIZE) {
            plat_log("portrait %s: not a %dx%d PNG, using the name box\n", path, PORTRAIT_SIZE, PORTRAIT_SIZE);
        } else if ((rgba = stbi_load_from_memory(file, (int) size, &w, &h, &channels, 4)) == NULL ||
                   w != PORTRAIT_SIZE || h != PORTRAIT_SIZE) {
            plat_log("portrait %s: could not be read, using the name box\n", path);
            stbi_image_free(rgba);
        } else {
            out->width = w;
            out->height = h;
            out->rgba = malloc((unsigned long) w * h * 4);
            if (out->rgba != NULL) {
                memcpy(out->rgba, rgba, (unsigned long) w * h * 4);
                halve(out);
            }
            stbi_image_free(rgba);
        }
    }
    free(file);
    fclose(f);
}

static void load_race_art(void) {
    // The portraits are in a different order from the characters (Character in enums.h).
    static const int sPortraitOfCharacter[10] = { 0, 1, 2, 4, 3, 5, 6, 7, 8, 9 };
    int i;
    for (i = 0; i < 10; i++) {
        if (load_texture(ASSET_TEX2D_HUD_KRUNCHICON + sPortraitOfCharacter[i], &sPortraits[i])) {
            halve(&sPortraits[i]);
        }
        load_texture(ASSET_TEX2D_HUD_TIMERNUMBER_0 + i, &sDigits[i]);
        load_texture(ASSET_TEX2D_HUD_TIMERNUMBERSMALL_0 + i, &sSmallDigits[i]);
        load_texture(ASSET_TEX2D_HUD_WEAPONICONROCKETNUMBERS_0 + i, &sItemCounts[i]);
    }
    for (i = 0; i < 15; i++) {
        load_texture(ASSET_TEX2D_HUD_WEAPONICONBOOST_0 + i, &sItems[i]);
    }
    for (i = 0; i < 8; i++) {
        load_texture(ASSET_TEX2D_HUD_PLACENUMBER_0 + i, &sPlaces[i]);
    }
    for (i = 0; i < 4; i++) {
        load_texture_frame(ASSET_TEX2D_HUD_PLACENUMBERSUFFIXES, i, &sSuffixes[i], NULL, NULL);
    }
    load_colon(&sDigits[1], &sColon, 3, 2, 9);
    load_colon(&sSmallDigits[1], &sSmallColon, 2, 2, 7);
    load_texture(ASSET_TEX2D_HUD_FORWARDSLASH, &sSlash);
    load_texture(ASSET_TEX2D_HUD_LAP, &sLapLabel);
    load_texture(ASSET_TEX2D_HUD_TIME, &sTimeLabel);
    load_texture(ASSET_TEX2D_HUD_FINALSMALL, &sFinalLabel);
    load_texture(ASSET_TEX2D_HUD_LAPSMALL, &sLapSmallLabel);
    load_texture(ASSET_TEX2D_HUD_FINISHSMALL, &sFinishLabel);
    load_banana();
    load_texture(ASSET_TEX2D_HUD_XSMALL, &sTimes);
    for (i = 0; i < RACE_HUD_SPECIALS; i++) {
        load_special_portrait(i, &sSpecialPortraits[i]);
    }
}

/* A picture at a whole-number size (nearest pixel, so it stays sharp), with a one pixel dark outline. */
static void picture_scaled(const Picture *p, int x, int y, int scale, int outline) {
    int i, j, k;
    static const int sOffsets[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    if (p->rgba == NULL) {
        return;
    }
    for (k = outline ? 0 : 4; k <= 4; k++) {
        for (j = 0; j < p->height * scale; j++) {
            for (i = 0; i < p->width * scale; i++) {
                const unsigned char *t = p->rgba + ((j / scale) * p->width + i / scale) * 4;
                if (t[3] == 0) {
                    continue;
                }
                if (k < 4) {
                    plot(x + i + sOffsets[k][0], y + j + sOffsets[k][1], 0, 4, 30, t[3] * 150 / 255);
                } else {
                    plot(x + i, y + j, t[0], t[1], t[2], t[3]);
                }
            }
        }
    }
}

/* The leftmost and rightmost columns a picture uses, so pieces can be packed by what they show. */
static void used_columns(const Picture *p, int *first, int *end) {
    int x, y;
    *first = p->width;
    *end = 0;
    for (y = 0; p->rgba != NULL && y < p->height; y++) {
        for (x = 0; x < p->width; x++) {
            if (p->rgba[(y * p->width + x) * 4 + 3] > 40) {
                if (x < *first) {
                    *first = x;
                }
                if (x + 1 > *end) {
                    *end = x + 1;
                }
            }
        }
    }
    if (*end <= *first) {
        *first = 0;
        *end = p->width;
    }
}

/* Draws a picture by its used columns from x, and returns where the next piece goes (gap pixels on). */
static int piece(const Picture *p, int x, int y, int gap) {
    int first, end;
    used_columns(p, &first, &end);
    picture_scaled(p, x - first, y, 1, 1);
    return x + end - first + gap;
}

/* A time as the game shows it, MM:SS:HH (hud_timer_render), from 60ths of a second. */
static int race_time(int x, int y, int frames, int small) {
    int hundredths, seconds, minutes, digits[6], i;
    const Picture *set = small ? sSmallDigits : sDigits;
    const Picture *colon = small ? &sSmallColon : &sColon;
    if (frames < 0) {
        frames = 0;
    }
    if (frames > 60 * 60 * 100 - 1) {
        frames = 60 * 60 * 100 - 1;
    }
    hundredths = (frames % 60) * 100 / 60;
    seconds = (frames / 60) % 60;
    minutes = frames / 3600;
    digits[0] = minutes / 10;
    digits[1] = minutes % 10;
    digits[2] = seconds / 10;
    digits[3] = seconds % 10;
    digits[4] = hundredths / 10;
    digits[5] = hundredths % 10;
    for (i = 0; i < 6; i++) {
        x = piece(&set[digits[i]], x, y, 1);
        if (i == 1 || i == 3) {
            x = piece(colon, x, y, 1);
        }
    }
    return x;
}

/* A gap as "+1.42" (ahead of the player) or "-0.61", "+1 lap", or "" before the first checkpoint. */
static void format_gap(int gap, char *out, int size) {
    int value;
    if (gap == RACE_HUD_NO_GAP) {
        out[0] = '\0';
        return;
    }
    if (gap >= RACE_HUD_LAPPED || gap <= -RACE_HUD_LAPPED) {
        int laps = gap > 0 ? gap - RACE_HUD_LAPPED : -gap - RACE_HUD_LAPPED;
        snprintf(out, size, "%c%d lap%s", gap > 0 ? '+' : '-', laps, laps == 1 ? "" : "s");
        return;
    }
    value = (gap < 0 ? -gap : gap) * 100 / 60;
    if (value > 9999) {
        value = 9999;
    }
    snprintf(out, size, "%c%d.%02d", gap >= 0 ? '+' : '-', value / 100, value % 100);
}

/* The track's map from the ROM: its sprite gives the texture and where the texture sits in the sprite. */
static void load_map(int sprite, int mirrored, const unsigned char *colour) {
    const unsigned char *table, *data;
    unsigned long tableSize, dataSize, start;
    Picture tex = { 0, 0, NULL };
    int texture, outW, outH, x, y, pad = 3;
    unsigned char *alpha = NULL;

    free(sMap.rgba);
    sMap.rgba = NULL;
    sMapSprite = sprite;
    sMapMirrored = mirrored;
    memcpy(sMapColour, colour, 3);
    table = section(ASSET_SPRITES_TABLE, &tableSize);
    data = section(ASSET_SPRITES, &dataSize);
    if (sprite < 0 || table == NULL || data == NULL || (unsigned long) (sprite + 2) * 4 > tableSize) {
        return;
    }
    start = read_be32(table + sprite * 4);
    if (start + 13 > dataSize) {
        return;
    }
    // SpriteAsset: the first texture, the frames, the anchor, then each frame's first texture.
    texture = (short) read_be16(data + start) + data[start + 12];
    if (!load_texture_frame(texture, 0, &tex, &sMapTexX, &sMapTexY)) {
        return;
    }
    sMapTexWidth = tex.width;
    // As large as the panel allows, scaled smoothly: the map is a mask, so its edge is redrawn sharp below.
    sMapScale = (float) (RIGHT_X1 - RIGHT_X0 - 10) / tex.width;
    if ((float) (LOWER_Y1 - LOWER_Y0 - 10) / tex.height < sMapScale) {
        sMapScale = (float) (LOWER_Y1 - LOWER_Y0 - 10) / tex.height;
    }
    outW = (int) (tex.width * sMapScale) + pad * 2;
    outH = (int) (tex.height * sMapScale) + pad * 2;
    sMap.rgba = calloc(outW * outH, 4);
    alpha = calloc(outW * outH, 1);
    if (sMap.rgba == NULL || alpha == NULL) {
        free(sMap.rgba);
        sMap.rgba = NULL;
        free(alpha);
        free(tex.rgba);
        return;
    }
    sMap.width = outW;
    sMap.height = outH;
    for (y = 0; y < outH; y++) {
        for (x = 0; x < outW; x++) {
            // Bilinear, from the centre of the output pixel.
            float u = (x - pad + 0.5f) / sMapScale - 0.5f, v = (y - pad + 0.5f) / sMapScale - 0.5f;
            int u0 = (int) (u + 1) - 1, v0 = (int) (v + 1) - 1, c, i;
            float fu = u - u0, fv = v - v0, sum[4] = { 0, 0, 0, 0 };
            unsigned char *o = sMap.rgba + (y * outW + x) * 4;
            int a;
            for (i = 0; i < 4; i++) {
                int su = u0 + (i & 1), sv = v0 + (i >> 1);
                float w = ((i & 1) ? fu : 1 - fu) * ((i >> 1) ? fv : 1 - fv);
                const unsigned char *t;
                if (mirrored) {
                    su = tex.width - 1 - su;
                }
                if (su < 0 || sv < 0 || su >= tex.width || sv >= tex.height) {
                    continue;
                }
                t = tex.rgba + (sv * tex.width + su) * 4;
                for (c = 0; c < 3; c++) {
                    sum[c] += t[c] * t[3] * w;
                }
                sum[3] += t[3] * w;
            }
            // A sharp edge: the soft alpha is stretched so the edge is about one pixel wide.
            a = (int) ((sum[3] - 70) * 255 / 80);
            a = a < 0 ? 0 : a > 255 ? 255 : a;
            alpha[y * outW + x] = (unsigned char) a;
            for (c = 0; c < 3; c++) {
                // The light track with a touch of the level's own map colour.
                int lum = sum[3] > 0 ? (int) (sum[c] / sum[3]) : 0;
                int tint = (255 * 7 + colour[c]) / 8;
                o[c] = (unsigned char) (lum * tint / 255);
            }
            o[3] = (unsigned char) a;
        }
    }
    // A dark rim, two pixels wide, so the track stands off the panel.
    for (y = 0; y < outH; y++) {
        for (x = 0; x < outW; x++) {
            unsigned char *o = sMap.rgba + (y * outW + x) * 4;
            int i, j, rim = 0;
            for (j = -2; j <= 2; j++) {
                for (i = -2; i <= 2; i++) {
                    int xx = x + i, yy = y + j;
                    if (xx >= 0 && yy >= 0 && xx < outW && yy < outH && alpha[yy * outW + xx] > rim) {
                        rim = alpha[yy * outW + xx];
                    }
                }
            }
            rim = rim * 200 / 255;
            if (o[3] < 255 && rim > 0) {
                // The track over its rim.
                int a = o[3], out = a + rim * (255 - a) / 255, c;
                static const unsigned char sRim[3] = { 0, 10, 60 };
                for (c = 0; c < 3; c++) {
                    o[c] = out > 0 ? (unsigned char) ((o[c] * a + sRim[c] * rim * (255 - a) / 255) / out) : 0;
                }
                o[3] = (unsigned char) out;
            }
        }
    }
    sMapX = RIGHT_X0 + (RIGHT_X1 - RIGHT_X0 - outW) / 2;
    sMapY = LOWER_Y0 + (LOWER_Y1 - LOWER_Y0 - outH) / 2;
    free(alpha);
    free(tex.rgba);
}

/*
 * Where a racer's map position (race_hud.h: sprite pixels times 4) is on the screen. The position is off by about a
 * pixel from where the game's marker sprites show (their anchor): moved back by this much, racers sit on the track
 * all the way round (measured on PC on two tracks).
 */
static void map_point(const RaceHudRacer *r, int *x, int *y) {
    float u = r->mapX / 4.0f - sMapTexX - 1.5f, v = r->mapY / 4.0f - sMapTexY - 1.0f;
    if (sMapMirrored) {
        u = sMapTexWidth - u;
    }
    *x = sMapX + 3 + (int) (u * sMapScale);
    *y = sMapY + 3 + (int) (v * sMapScale);
}

static void dot(int cx, int cy, Colour c) {
    int x, y;
    for (y = -3; y <= 3; y++) {
        for (x = -3; x <= 3; x++) {
            int d = x * x + y * y;
            if (d <= 6) {
                plot(cx + x, cy + y, c.r, c.g, c.b, 255);
            } else if (d <= 11) {
                plot(cx + x, cy + y, 0, 4, 30, 220);
            }
        }
    }
    plot(cx - 1, cy - 1, 255, 255, 255, 170);
}

/* The player's arrow: the HUD's gold, pointing along heading (0 up, counter-clockwise). */
static void arrow_marker(int cx, int cy, unsigned heading) {
    static const float sShape[3][2] = { { 0, -7 }, { 5, 5 }, { -5, 5 } };
    float angle = heading * (6.2831853f / 65536.0f), c = cosf(angle), s = sinf(angle), p[3][2];
    int i, x, y, pass;
    for (i = 0; i < 3; i++) {
        // Counter-clockwise on a screen whose y grows downwards.
        p[i][0] = sShape[i][0] * c + sShape[i][1] * s;
        p[i][1] = -sShape[i][0] * s + sShape[i][1] * c;
    }
    for (pass = 0; pass < 2; pass++) {
        // An outline (the triangle one pixel bigger, dark), then the gold.
        float grow = pass == 0 ? 1.25f : 1.0f;
        for (y = -9; y <= 9; y++) {
            for (x = -9; x <= 9; x++) {
                int inside = 1;
                for (i = 0; i < 3 && inside; i++) {
                    const float *a = p[i], *b = p[(i + 1) % 3];
                    float cross = (b[0] - a[0]) * (y / grow - a[1]) - (b[1] - a[1]) * (x / grow - a[0]);
                    inside = cross >= 0;
                }
                if (inside) {
                    if (pass == 0) {
                        plot(cx + x, cy + y, 60, 20, 0, 230);
                    } else {
                        plot(cx + x, cy + y, y < 0 ? 255 : 240, y < 0 ? 236 : 190, y < 0 ? 90 : 20, 255);
                    }
                }
            }
        }
    }
}

/* The parts that only change with the track: the background, the panels and the map. */
static void make_race_base(const RaceHud *hud) {
    unsigned short *image = sImage;
    int top = sClipTop, end = sClipEnd;
    if (hud->mapSprite != sMapSprite || hud->mapMirrored != sMapMirrored || memcmp(hud->mapColour, sMapColour, 3)) {
        load_map(hud->mapSprite, hud->mapMirrored, hud->mapColour);
    } else if (sRaceBaseMade) {
        return;
    }
    sImage = sRaceBase;
    sClipTop = 0;
    sClipEnd = RACE_ROWS_Y;
    memcpy(sRaceBase, sBackground, sizeof(sRaceBase));
    box(TOWER_X0, UPPER_Y0, TOWER_X1, LOWER_Y1, BOX_FILL, BOX_EDGE);
    box(MIDDLE_X0, UPPER_Y0, MIDDLE_X1, UPPER_Y1, BOX_FILL, BOX_EDGE);
    box(RIGHT_X0, UPPER_Y0, RIGHT_X1, UPPER_Y1, BOX_FILL, BOX_EDGE);
    box(MIDDLE_X0, LOWER_Y0, MIDDLE_X1, LOWER_Y1, BOX_FILL, BOX_EDGE);
    box(RIGHT_X0, LOWER_Y0, RIGHT_X1, LOWER_Y1, BOX_FILL, BOX_EDGE);
    if (sMap.rgba != NULL) {
        picture(&sMap, sMapX, sMapY);
    }
    sRaceBaseMade = 1;
    sImage = image;
    sClipTop = top;
    sClipEnd = end;
}

/* The colours of the bosses and Taj, for their frames and map dots (RaceHudRacer.special). */
static const Colour sSpecialColours[RACE_HUD_SPECIALS] = {
    { 0, 175, 0, 255 }, { 0, 128, 255, 255 }, { 255, 64, 32, 255 }, { 255, 128, 0, 255 }, { 255, 128, 190, 255 },
    { 160, 64, 255, 255 },
};

/* A racer's colour; 0 if it has neither a portrait nor a colour (an unknown character). */
static int racer_colour(const RaceHudRacer *r, Colour *out) {
    if (r->special > RACE_HUD_NOT_SPECIAL && r->special < RACE_HUD_SPECIALS) {
        *out = sSpecialColours[r->special];
        return 1;
    }
    if (r->special == RACE_HUD_NOT_SPECIAL && r->character >= 0 && r->character < 10) {
        *out = sCharacterColours[r->character];
        return 1;
    }
    return 0;
}

static void draw_tower(const RaceHud *hud) {
    int i;
    for (i = 0; i < hud->racerCount && i < RACE_HUD_RACERS; i++) {
        const RaceHudRacer *r = &hud->racers[i];
        int y = TOWER_ROW_Y + i * TOWER_ROW_HEIGHT, character = r->character;
        char number[4], gap[16];
        Colour colour;
        if (r->player) {
            // The player's row: lit in the HUD's gold.
            box(TOWER_X0 + 2, y - 1, TOWER_X1 - 2, y + TOWER_ROW_HEIGHT - 1, (Colour) { 255, 200, 40, 70 }, GOLD);
        }
        snprintf(number, sizeof(number), "%d", i + 1);
        text_centred(FONT_FUN, TOWER_X0 + 3, TOWER_X0 + 19, y + 5, number, WHITE, 1);
        if (racer_colour(r, &colour)) {
            // The portrait in a frame of the racer's map colour, so the map's dots can be matched to faces.
            fill(25, y + 1, 47, y + 23, colour);
            fill(24, y, 48, y + 1, (Colour) { 0, 4, 30, 200 });
            fill(24, y + 23, 48, y + 24, (Colour) { 0, 4, 30, 200 });
            if (r->special == RACE_HUD_NOT_SPECIAL) {
                picture(&sPortraits[character], 26, y + 2);
            } else if (sSpecialPortraits[r->special].rgba != NULL) {
                picture(&sSpecialPortraits[r->special], 26, y + 2);
            } else {
                // The name box: the boss's initial on its colour.
                char initial[2] = { (char) (sSpecialNames[r->special][0] - 'a' + 'A'), 0 };
                fill(26, y + 2, 46, y + 22, (Colour) { 0, 20, 90, 255 });
                text_centred(FONT_FUN, 26, 46, y + 5, initial, WHITE, 1);
            }
        }
        if (r->player) {
            text(FONT_SMALL, 53, y + 6, "You", YELLOW, 1);
        } else {
            format_gap(r->gap, gap, sizeof(gap));
            text_clipped(FONT_SMALL, 52, y + 6, gap, r->gap > 0 ? AHEAD : BEHIND, 1, TOWER_X1 - 2);
        }
    }
}

static void draw_item(const RaceHud *hud) {
    int index = hud->item * 3 + hud->itemLevel;
    // The slot, then the item at the N64's own size (40 pixels, as sharp as the art is) in it.
    box(117, 24, 165, 72, (Colour) { 0, 0, 20, 60 }, (Colour) { 140, 180, 255, 70 });
    if (hud->item < 0 || index < 0 || index >= 15 || sItems[index].rgba == NULL) {
        return;
    }
    picture_scaled(&sItems[index], 121, 28, 1, 1);
    if (hud->itemCount > 1 && hud->itemCount <= 10) {
        piece(&sItemCounts[hud->itemCount - 1], 150, 60, 0);
    }
}

static void draw_standing(const RaceHud *hud) {
    int x = RIGHT_X0 + 8, y;
    if (hud->position >= 1 && hud->position <= 8) {
        int suffix = hud->position <= 3 ? hud->position - 1 : 3;
        x = piece(&sPlaces[hud->position - 1], x, 9, 1);
        piece(&sSuffixes[suffix], x, 11, 0);
    }
    if (hud->racerCount > 0) {
        char of[8];
        snprintf(of, sizeof(of), "of %d", hud->racerCount);
        text(FONT_SMALL, RIGHT_X1 - 8 - text_width(FONT_SMALL, of), 22, of, DIM, 1);
    }
    y = 44;
    if (hud->finished) {
        piece(&sFinishLabel, RIGHT_X0 + 8, y, 0);
    } else if (hud->laps > 0 && hud->lap >= hud->laps && hud->laps > 1) {
        x = piece(&sFinalLabel, RIGHT_X0 + 6, y, 3);
        piece(&sLapSmallLabel, x, y, 0);
    } else if (hud->laps > 0) {
        x = piece(&sLapLabel, RIGHT_X0 + 8, y + 3, 4);
        x = piece(&sDigits[hud->lap % 10], x, y, 1);
        x = piece(&sSlash, x, y + 1, 1);
        piece(&sDigits[hud->laps % 10], x, y, 0);
    }
    x = piece(&sTimeLabel, RIGHT_X0 + 8, 72, 4);
    race_time(x, 70, hud->raceTime, 1);
}

static void draw_bananas_and_laps(const RaceHud *hud) {
    int x, best = -1, i, last = hud->lapsTimed - 2;
    char label[16];
    picture_scaled(&sBanana, MIDDLE_X0 + 6, LOWER_Y0 + 3, 1, 1);
    x = piece(&sTimes, MIDDLE_X0 + 42, LOWER_Y0 + 17, 2);
    x = piece(&sDigits[(hud->bananas / 10) % 10], x, LOWER_Y0 + 14, 1);
    piece(&sDigits[hud->bananas % 10], x, LOWER_Y0 + 14, 0);
    // The last finished lap (or the lap so far, on the first) and the best finished lap.
    for (i = 0; i < hud->lapsTimed - 1 && i < RACE_HUD_LAPS; i++) {
        if (best < 0 || hud->lapTimes[i] < best) {
            best = hud->lapTimes[i];
        }
    }
    if (last >= 0) {
        snprintf(label, sizeof(label), "Lap %d", last + 1);
        text(FONT_SMALL, MIDDLE_X0 + 44, LOWER_Y0 + 42, label, HINT, 1);
        race_time(MIDDLE_X0 + 8, LOWER_Y0 + 58, hud->lapTimes[last], 1);
    } else if (hud->lapsTimed > 0) {
        snprintf(label, sizeof(label), "Lap %d", hud->lapsTimed);
        text(FONT_SMALL, MIDDLE_X0 + 44, LOWER_Y0 + 42, label, HINT, 1);
        race_time(MIDDLE_X0 + 8, LOWER_Y0 + 58, hud->lapTimes[hud->lapsTimed - 1], 1);
    }
    text(FONT_SMALL, MIDDLE_X0 + 8, LOWER_Y0 + 76, "Best lap", HINT, 1);
    if (best >= 0) {
        race_time(MIDDLE_X0 + 8, LOWER_Y0 + 88, best, 1);
    } else {
        text(FONT_SMALL, MIDDLE_X0 + 8, LOWER_Y0 + 88, "--", DIM, 1);
    }
}

static void draw_map_markers(const RaceHud *hud) {
    int i, x, y;
    if (sMap.rgba == NULL) {
        return;
    }
    // The others first, last place first, so the leaders are on top; then the player.
    for (i = hud->racerCount - 1; i >= 0; i--) {
        const RaceHudRacer *r = &hud->racers[i];
        Colour colour;
        if (!r->player && r->onMap && racer_colour(r, &colour)) {
            map_point(r, &x, &y);
            dot(x, y, colour);
        }
    }
    for (i = 0; i < hud->racerCount; i++) {
        const RaceHudRacer *r = &hud->racers[i];
        if (r->player && r->onMap) {
            map_point(r, &x, &y);
            arrow_marker(x, y, r->heading);
        }
    }
}

static void draw_race(const State *s) {
    const RaceHud *hud = &s->race;
    make_race_base(hud);
    memcpy(sImage, sRaceBase, sizeof(sRaceBase));
    draw_tower(hud);
    draw_item(hud);
    draw_standing(hud);
    draw_bananas_and_laps(hud);
    draw_map_markers(hud);
}

void bs_area_rows(int area, int *first, int *end) {
    static const int sRows[BS_AREA_COUNT + 1] = { 0, BS_BUTTONS_Y, BS_STATUS_Y, BS_HEIGHT };
    *first = sRows[area];
    *end = sRows[area + 1];
}

void bs_draw(unsigned short *image, int area) {
    if (!sBackgroundMade) {
        make_background();
    }
    sImage = image;
    bs_area_rows(area, &sClipTop, &sClipEnd);
    memcpy(image + sClipTop * BS_WIDTH, sBackground + sClipTop * BS_WIDTH,
           (sClipEnd - sClipTop) * BS_WIDTH * sizeof(unsigned short));
    switch (area) {
        case BS_AREA_TOP:
            if (sDrawn.menuShown) {
                draw_menu(&sDrawn);
            } else if (sDrawn.raceShown && sDrawn.overlayLines == 0) {
                draw_race(&sDrawn);
            } else {
                draw_home(&sDrawn);
            }
            break;
        case BS_AREA_BUTTONS:
            draw_buttons(&sDrawn);
            break;
        default:
            draw_status(&sDrawn);
            break;
    }
}
