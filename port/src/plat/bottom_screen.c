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
static int load_texture(int id, Picture *out) {
    const unsigned char *table, *data, *pixels;
    unsigned char *unpacked = NULL;
    unsigned long tableSize, dataSize, start, end, need;
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
    need = 0x20 + (unsigned long) width * height * bytes;
    if (bytes == 0 || width == 0 || height == 0) {
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
        pixels = unpacked + 0x20;
    } else {
        if (end - start < need) {
            return 0;
        }
        pixels = data + 0x20;
    }
    out->rgba = malloc(width * height * 4);
    if (out->rgba == NULL) {
        free(unpacked);
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
    return 1;
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

/* A value of "On" or "Off" is shown with the game's ON and OFF labels, the chosen one lit. */
static int draw_on_off(const char *value, int x, int y) {
    int on = strcmp(value, "On") == 0;
    if ((!on && strcmp(value, "Off") != 0) || sOnOff[0][0].rgba == NULL || sOnOff[0][1].rgba == NULL ||
        sOnOff[1][0].rgba == NULL || sOnOff[1][1].rgba == NULL) {
        return 0;
    }
    picture(&sOnOff[0][on], x, y);
    picture(&sOnOff[1][!on], x + sOnOff[0][on].width + 6, y);
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
                     row->value[0] == '\0' ? BS_WIDTH - 24 : onOff ? 204 : BS_MENU_VALUE_X + 4);
        if (row->value[0] == '\0') {
            arrow(0, BS_WIDTH - 22, y + 6);
        } else if (!draw_on_off(row->value, 206, y + 3)) {
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
