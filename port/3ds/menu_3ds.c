/*
 * The Settings menu on the bottom screen. The panel's "Settings" button (update_3ds.c) opens it over the top of the
 * screen (plat_3ds_menu_draw; plat/bottom_screen.c draws it in the game's style). A page is a list of rows, each a
 * label with its value between arrows (or the game's ON and OFF labels; or just an arrow for a row that does
 * something or opens a page), and a hint under it. Tapping the left half of a value steps it back, anywhere else on
 * the row steps it forward; taps sound like the game's menus. The panel shows Close (Back on another page), and
 * Prev / Next when a page has more rows than fit.
 *
 * Other parts of the port add a page with menu_3ds_add_page; the main page gets a row that opens it. Settings are
 * saved in settings.txt (plat/settings.h) or a file of their owner's. Everything here runs on the bottom screen's
 * thread (update_3ds.c), so a page's functions are called from that thread.
 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "menu_3ds.h"
#include "plat/bottom_screen.h"
#include "plat/plat.h"

void plat_3ds_menu_draw(const BsMenu *menu);
void plat_3ds_set_antialias_setting(int on);
int plat_3ds_antialias_setting(void);
void plat_3ds_set_mipmap_setting(int mode);
int plat_3ds_mipmap_setting(void);
void plat_3ds_set_colour_setting(int level);
int plat_3ds_colour_setting(void);
void plat_3ds_set_screen_setting(int which, int on);
int plat_3ds_screen_setting(int which);
int plat_3ds_wide800_supported(void);
long port_autorun_enabled(void);
void port_autorun_request(long start);
int prof_user_enabled(void);
void prof_set_user_enabled(int on);

typedef struct MenuPage {
    const char *title;
    int (*count)(void);
    const char *(*label)(int i);
    void (*value)(int i, char *out, int size); /* An empty value: the row does something, or opens a page. */
    void (*change)(int i, int step);           /* step is -1 or +1. */
    const char *(*hint)(int i);                /* Optional; NULL or "" for none. */
    const char *note;                          /* Optional line under the rows. */
} MenuPage;

/* The rows of a page shown at once (the screen's geometry is in bottom_screen.h). */
#define ROWS_PER_PAGE BS_MENU_ROWS

#define MAX_PAGES 8
static MenuPage sPages[MAX_PAGES];
static int sPageCount;

static const MenuPage *sPage;
static int sFirst;  // First row shown.
static int sResult; // What a tap asked of the panel (MENU_*).
static int sDrawnTestRun = -1;

static const MenuPage sMainPage, sTestRunPage, sScreenPage;

static void draw(void);

static void open_page(const MenuPage *page) {
    sPage = page;
    sFirst = 0;
    draw();
}

/* The main page: anti-aliasing, mipmaps, colour boost, the screen page, the profiler, the added pages, then the test
 * run, updates and bug report. */
enum { MAIN_AA, MAIN_MIPMAPS, MAIN_COLOUR, MAIN_SCREEN, MAIN_PROFILER, MAIN_PAGES };

static const char *const sColourLevels[4] = { "Off", "Low", "Medium", "High" };
static const char *const sMipmapModes[3] = { "Off", "On", "Show" };

static int main_count(void) {
    return MAIN_PAGES + sPageCount + 3;
}

static const char *main_label(int i) {
    static const char *const sLast[3] = { "Test run", "Check for updates", "Send a bug report" };
    if (i == MAIN_AA) {
        return "Anti-aliasing";
    } else if (i == MAIN_MIPMAPS) {
        return "Mipmaps";
    } else if (i == MAIN_COLOUR) {
        return "Colour boost";
    } else if (i == MAIN_SCREEN) {
        return "Screen";
    } else if (i == MAIN_PROFILER) {
        return "Profiler";
    } else if (i < MAIN_PAGES + sPageCount) {
        return sPages[i - MAIN_PAGES].title;
    }
    return sLast[i - MAIN_PAGES - sPageCount];
}

static void main_value(int i, char *out, int size) {
    if (i == MAIN_AA) {
        snprintf(out, size, "%s", plat_3ds_antialias_setting() ? "On" : "Off");
    } else if (i == MAIN_MIPMAPS) {
        snprintf(out, size, "%s", sMipmapModes[plat_3ds_mipmap_setting()]);
    } else if (i == MAIN_COLOUR) {
        snprintf(out, size, "%s", sColourLevels[plat_3ds_colour_setting()]);
    } else if (i == MAIN_PROFILER) {
        snprintf(out, size, "%s", prof_user_enabled() ? "On" : "Off");
    } else {
        out[0] = '\0';
    }
}

static const char *main_hint(int i) {
    static const char *const sLast[3] = { NULL, "Install a newer build from GitHub",
                                          "Log and snapshot (also SELECT+touch)" };
    if (i == MAIN_AA) {
        return "Smoother edges; the GPU draws 4x";
    } else if (i == MAIN_MIPMAPS) {
        return plat_3ds_mipmap_setting() == 2 ? "Tints each level a colour" : "No shimmer on distant textures";
    } else if (i == MAIN_COLOUR) {
        return "Richer colours on the 3DS screen";
    } else if (i == MAIN_SCREEN) {
        return "Widescreen, 800 pixels, 3D";
    } else if (i == MAIN_PROFILER) {
        return "Frame times on this screen";
    } else if (i < MAIN_PAGES + sPageCount) {
        return NULL;
    } else if (i == MAIN_PAGES + sPageCount) {
        return port_autorun_enabled() ? "Running now: open to stop it" : "AI races that look for problems";
    }
    return sLast[i - MAIN_PAGES - sPageCount];
}

static void main_change(int i, int step) {
    if (i == MAIN_AA) {
        plat_3ds_set_antialias_setting(!plat_3ds_antialias_setting());
    } else if (i == MAIN_MIPMAPS) {
        plat_3ds_set_mipmap_setting((plat_3ds_mipmap_setting() + step + 3) % 3);
    } else if (i == MAIN_COLOUR) {
        plat_3ds_set_colour_setting((plat_3ds_colour_setting() + step + 4) % 4);
    } else if (i == MAIN_SCREEN) {
        open_page(&sScreenPage);
    } else if (i == MAIN_PROFILER) {
        prof_set_user_enabled(!prof_user_enabled());
    } else if (i < MAIN_PAGES + sPageCount) {
        open_page(&sPages[i - MAIN_PAGES]);
    } else if (i == MAIN_PAGES + sPageCount) {
        open_page(&sTestRunPage);
    } else if (i == MAIN_PAGES + sPageCount + 1) {
        sResult = MENU_UPDATES;
    } else {
        sResult = MENU_BUGREPORT;
    }
}

static const MenuPage sMainPage = { "Settings", main_count, main_label, main_value, main_change, main_hint, NULL };

/* The top screen (roadmap phase 6, gfx_c3d.c): widescreen, the 800 pixel mode and stereo 3D, each on or off. */
static int screen_count(void) {
    return 3;
}

static const char *screen_label(int i) {
    static const char *const sLabels[3] = { "Widescreen", "800 pixels", "Stereo 3D" };
    return sLabels[i];
}

static const char *const sWideStyles[4] = { "Off", "Wide", "Balanced", "Zoomed" };

static void screen_value(int i, char *out, int size) {
    if (i == 0) {
        snprintf(out, size, "%s", sWideStyles[plat_3ds_screen_setting(0)]);
    } else {
        snprintf(out, size, "%s", plat_3ds_screen_setting(i) ? "On" : "Off");
    }
}

static const char *screen_hint(int i) {
    if (i == 0) {
        static const char *const sHints[4] = { "4:3 picture in the middle", "Most to the sides; edges stretch most",
                                               "Between Wide and Zoomed", "4:3's sides, less top; no extra stretch" };
        return sHints[plat_3ds_screen_setting(0)];
    } else if (i == 1) {
        return plat_3ds_wide800_supported() ? "Sharper picture while 3D is off" : "Not on this console (2DS)";
    }
    return "With the 3D slider; no anti-aliasing in 3D";
}

static void screen_change(int i, int step) {
    if (i == 0) {
        plat_3ds_set_screen_setting(0, (plat_3ds_screen_setting(0) + step + 4) % 4);
    } else {
        plat_3ds_set_screen_setting(i, !plat_3ds_screen_setting(i));
    }
}

static const MenuPage sScreenPage = {
    "Screen", screen_count, screen_label, screen_value, screen_change, screen_hint, "Changes show from the next frame"
};

/* The test run (port_autorun.c): stop the one running, or start one of its four kinds. */
static int test_count(void) {
    return port_autorun_enabled() ? 1 : 4;
}

static const char *test_label(int i) {
    static const char *const sStart[4] = { "Full test", "Quick test", "Boss fights", "AA bench" };
    return port_autorun_enabled() ? "Stop the test run" : sStart[i];
}

static void test_value(int i, char *out, int size) {
    (void) i;
    (void) size;
    out[0] = '\0';
}

static const char *test_hint(int i) {
    static const char *const sStart[4] = { "Every track and character, ~8 hours", "5 slowest tracks twice, ~20 minutes",
                                           "All 10 boss races once, ~30 minutes", "Heavy races, AA off and on, ~30 min" };
    return port_autorun_enabled() ? "The results so far go out as a report" : sStart[i];
}

static void test_change(int i, int step) {
    (void) step;
    if (port_autorun_enabled()) {
        port_autorun_request(0);
    } else {
        // port_autorun_request: 1 the full run, 3 the quick test, 4 the boss fights, 2 the anti-aliasing benchmark.
        static const long sRequests[4] = { 1, 3, 4, 2 };
        port_autorun_request(sRequests[i]);
    }
    // The run turns on or off at the game's next frame.
    svcSleepThread(200 * 1000 * 1000);
    sResult = MENU_CLOSE;
}

static const MenuPage sTestRunPage = {
    "Test run", test_count, test_label, test_value, test_change, test_hint, "Starts at the title; saves not written"
};

void menu_3ds_add_page(const char *title, int (*count)(void), const char *(*label)(int i),
                       void (*value)(int i, char *out, int size), void (*change)(int i, int step)) {
    MenuPage *page;
    if (sPageCount >= MAX_PAGES) {
        plat_log("menu: no room for the %s page", title);
        return;
    }
    page = &sPages[sPageCount];
    memset(page, 0, sizeof(*page));
    page->title = title;
    page->count = count;
    page->label = label;
    page->value = value;
    page->change = change;
    sPageCount++;
}

/* Drawing. */

static void draw(void) {
    int count = sPage->count(), slot;
    BsMenu menu;

    if (sFirst >= count) {
        sFirst = 0;
    }
    memset(&menu, 0, sizeof(menu));
    snprintf(menu.title, sizeof(menu.title), "%s", sPage->title);
    if (count > ROWS_PER_PAGE) {
        snprintf(menu.pages, sizeof(menu.pages), "%d/%d", sFirst / ROWS_PER_PAGE + 1,
                 (count + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE);
    }
    for (slot = 0; slot < ROWS_PER_PAGE && sFirst + slot < count; slot++) {
        BsMenuRow *row = &menu.rows[slot];
        int i = sFirst + slot;
        const char *hint = sPage->hint != NULL ? sPage->hint(i) : NULL;
        snprintf(row->label, sizeof(row->label), "%s", sPage->label(i));
        sPage->value(i, row->value, sizeof(row->value));
        snprintf(row->hint, sizeof(row->hint), "%s", hint != NULL ? hint : "");
        menu.rowCount++;
    }
    if (sPage->note != NULL) {
        snprintf(menu.note, sizeof(menu.note), "%s", sPage->note);
    } else {
        snprintf(menu.note, sizeof(menu.note), "Build %.32s", plat_build_version());
    }
    plat_3ds_menu_draw(&menu);
    sDrawnTestRun = port_autorun_enabled() != 0;
}

/* The panel's calls. */

void menu_3ds_open(void) {
    open_page(&sMainPage);
}

void menu_3ds_close(void) {
    plat_3ds_menu_draw(NULL);
    sPage = NULL;
}

int menu_3ds_on_main_page(void) {
    return sPage == &sMainPage;
}

/* Back to the main page. Returns 0 if this was the main page already. */
int menu_3ds_back(void) {
    if (sPage == &sMainPage) {
        return 0;
    }
    open_page(&sMainPage);
    return 1;
}

/* Whether the page has rows before (direction -1) or after (+1) the ones shown. */
int menu_3ds_can_scroll(int direction) {
    return direction < 0 ? sFirst > 0 : sFirst + ROWS_PER_PAGE < sPage->count();
}

void menu_3ds_scroll(int direction) {
    if (menu_3ds_can_scroll(direction)) {
        sFirst += direction * ROWS_PER_PAGE;
        draw();
    }
}

/* A tap at (x, y) above the panel. Returns what the panel should do (MENU_*). */
int menu_3ds_tap(int x, int y) {
    int slot, i;
    char value[48];

    if (sPage == NULL || y < BS_MENU_FIRST_Y) {
        return MENU_STAY;
    }
    slot = (y - BS_MENU_FIRST_Y) / BS_MENU_ROW_HEIGHT;
    i = sFirst + slot;
    if (slot >= ROWS_PER_PAGE || i >= sPage->count()) {
        return MENU_STAY;
    }
    sPage->value(i, value, sizeof(value));
    sResult = MENU_STAY;
    // A value changing, or a row that does something or opens a page.
    plat_request_ui_sound(value[0] != '\0' ? PLAT_UI_SOUND_MOVE : PLAT_UI_SOUND_SELECT);
    sPage->change(i, value[0] != '\0' && x >= BS_MENU_VALUE_X && x < BS_MENU_VALUE_SPLIT ? -1 : 1);
    if (sResult == MENU_STAY && sPage != NULL) {
        // The page may have opened another one, which drew itself; this one redraws the changed row (all of them, as
        // a change can add or remove rows).
        draw();
    }
    return sResult;
}

/* Every few seconds: the test run can end by itself, which changes what its rows say. */
void menu_3ds_refresh(void) {
    if (sPage != NULL && sDrawnTestRun != (port_autorun_enabled() != 0)) {
        draw();
    }
}
