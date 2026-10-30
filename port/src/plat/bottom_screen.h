#ifndef PORT_PLAT_BOTTOM_SCREEN_H
#define PORT_PLAT_BOTTOM_SCREEN_H

/*
 * What the 3DS bottom screen shows, drawn in the game's own style (bottom_screen.c): its fonts, title logo and menu
 * pictures, read from the ROM. The screen is three areas, each redrawn on its own:
 * - the top (y 0-199): the logo, the controls and the newest messages, with the profiler's readout under them while
 *   it is on; or the Settings menu while it is open
 * - the buttons (y 200-223)
 * - the status lines (y 224-239), the watchdog's two lines
 * The platform code (plat_3ds.c) serialises the calls and copies the drawn image to the screen. It is platform
 * neutral so the host build can draw it to PNG files (DKR_BOTTOM_PREVIEW).
 */

#include "race_hud.h"
#include "setup_screen.h"

#define BS_WIDTH 320
#define BS_HEIGHT 240

enum { BS_AREA_TOP, BS_AREA_BUTTONS, BS_AREA_STATUS, BS_AREA_COUNT };
#define BS_BUTTONS_Y 200
#define BS_STATUS_Y 224

/*
 * The Settings menu (menu_3ds.c): its pages as tabs down the left, the chosen page's rows on the right, each value
 * between arrows, and under the rows the tip of the row touched last. Taps are mapped back with this geometry.
 */
#define BS_MENU_TABS 10
#define BS_MENU_TAB_X1 92 /* A tap left of this picks a tab. */
#define BS_MENU_TAB_Y0 4
#define BS_MENU_TAB_Y1 196
/* The height of each of count tabs. */
#define BS_MENU_TAB_HEIGHT(count) ((count) > 6 ? (BS_MENU_TAB_Y1 - BS_MENU_TAB_Y0) / (count) : 30)
#define BS_MENU_ROWS 5
#define BS_MENU_FIRST_Y 30
#define BS_MENU_ROW_HEIGHT 27
/* The value field of a row; a tap left of BS_MENU_VALUE_SPLIT steps the value back. */
#define BS_MENU_VALUE_X 214
#define BS_MENU_VALUE_SPLIT 261

typedef struct BsMenuRow {
    char label[40];
    char value[40]; /* Empty: the row does something or opens a page. */
    char hint[64];
} BsMenuRow;

typedef struct BsMenu {
    char title[32];
    char pages[16]; /* "1/2", or empty. */
    int tabCount, tab;
    char tabs[BS_MENU_TABS][16];
    int rowCount;
    BsMenuRow rows[BS_MENU_ROWS];
    int picked;    /* The row touched last, lit, with its hint as the tip; -1 for none. */
    char note[64]; /* The tip while no row with a hint is picked. */
} BsMenu;

/* monoFont: an 8x8 font of 256 characters, 8 bytes each, the highest bit leftmost (libctru's), for dense text. */
void bs_init(const unsigned char *monoFont);
/* Reads the fonts and pictures from the ROM. Call once, after the ROM is loaded and before the game's threads start. */
void bs_load_assets(void);

void bs_add_message(const char *text);
void bs_clear_messages(void);
/* The look: 0 Classic, 1 Glass, 2 Broadcast, 3 Dock (the same layout, drawn in another style). */
void bs_set_style(int style);
/* The profiler's readout, lines separated by '\n'; "" removes it. */
void bs_set_overlay(const char *text);
/* menu NULL closes it. */
void bs_show_menu(const BsMenu *menu);
/* Whether the menu or the overlay lines are up, so that the race HUD isn't drawn. */
int bs_race_hud_covered(void);
/*
 * The buttons, side by side: button i of count takes columns i * (40 / count) + 1 to (i + 1) * (40 / count) - 1 of
 * 8 pixels (update_3ds.c maps taps the same way). A label starting with '-' is a button that cannot be tapped (just
 * "-": a gap), one starting with '+' a switch that is on.
 */
void bs_set_buttons(const char *const *labels, int count);
/* A line of text in place of the buttons, while something is busy. */
void bs_set_panel_text(const char *text);
void bs_set_status_line(int line, const char *text);
/* The race HUD (race_hud.h) in place of the top area while racing, unless the menu or the profiler is showing; NULL
 * ends it. */
void bs_set_race_hud(const RaceHud *hud);

/* The first-run setup's page (setup_screen.h) over the top area and the buttons; NULL ends it. */
void bs_set_setup(const SetupScreen *setup);
/* What a tap at x, y on the setup's page hits: a tile, a button (SETUP_HIT_*), or SETUP_HIT_NONE. */
int bs_setup_hit(int x, int y);

/* The areas changed since the last call, as bits (1 << BS_AREA_*), cleared; and a copy of the state to draw. */
unsigned bs_take_changes(void);
/* Draws an area into a 320x240 RGB565 image (row by row, from the top left) from the state of the last take. */
void bs_draw(unsigned short *image, int area);
void bs_area_rows(int area, int *first, int *end);

#endif
