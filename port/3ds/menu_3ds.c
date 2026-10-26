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
#include "plat/options.h"
#include "plat/plat.h"

void plat_3ds_menu_draw(const BsMenu *menu);
void plat_3ds_set_antialias_setting(int aa);
int plat_3ds_antialias_setting(void);
void plat_3ds_set_fb16_setting(int on);
int plat_3ds_fb16_setting(void);
void plat_3ds_set_mipmap_setting(int mode);
int plat_3ds_mipmap_setting(void);
void plat_3ds_set_colour_setting(int level);
int plat_3ds_colour_setting(void);
void plat_3ds_set_screen_setting(int which, int on);
int plat_3ds_screen_setting(int which);
int plat_3ds_wide800_supported(void);
void plat_3ds_set_guard_band_setting(int on);
int plat_3ds_guard_band_setting(void);
long port_autorun_enabled(void);
void port_autorun_request(long start);
long port_autorun_switch_count(void);
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
static unsigned int sDrawnChanges; // options_change_count() when drawn: the game's menus change the same values.

static const MenuPage sMainPage, sTestRunPage, sScreenPage;

static void draw(void);

static void open_page(const MenuPage *page) {
    sPage = page;
    sFirst = 0;
    draw();
}

/* The main page: anti-aliasing, frame rate, 16-bit buffers, mipmaps, colour boost, the screen page, the profiler, the
 * added pages, then the test run, updates and bug report. A player's build (plat.h PLAT_DEBUG_TOOLS) shows only some
 * (main_hidden), so the page's own functions number the rows shown and the row_* ones every row. */
enum { MAIN_AA, MAIN_FPS, MAIN_FB16, MAIN_MIPMAPS, MAIN_COLOUR, MAIN_SCREEN, MAIN_AHEAD, MAIN_PROFILER, MAIN_PAGES };

/* Anti-aliasing in the order the row steps through (PLAT_AA_* values). */
static const int sAaOrder[3] = { PLAT_AA_OFF, PLAT_AA_2X, PLAT_AA_4X };
static const char *const sAaNames[3] = { "Off", "4x", "2x" }; // By PLAT_AA_* value

static const char *const sColourLevels[4] = { "Off", "Low", "Medium", "High" };
static const char *const sMipmapModes[3] = { "Off", "On", "Show" };

static int main_row_count(void) {
    return MAIN_PAGES + sPageCount + 3;
}

/* The performance switches and the debug tools: the test run, updates (which install test builds) and bug reports
 * (which need a GitHub token). */
static int main_hidden(int row) {
    return !PLAT_DEBUG_TOOLS &&
           (row == MAIN_FB16 || row == MAIN_AHEAD || row == MAIN_PROFILER || row >= MAIN_PAGES + sPageCount);
}

static const char *row_label(int i) {
    static const char *const sLast[3] = { "Test run", "Check for updates", "Send a bug report" };
    if (i == MAIN_AA) {
        return "Anti-aliasing";
    } else if (i == MAIN_FPS) {
        return "Frame rate";
    } else if (i == MAIN_FB16) {
        return "16-bit buffers";
    } else if (i == MAIN_MIPMAPS) {
        return "Mipmaps";
    } else if (i == MAIN_COLOUR) {
        return "Colour boost";
    } else if (i == MAIN_SCREEN) {
        return "Screen";
    } else if (i == MAIN_AHEAD) {
        return "Render ahead";
    } else if (i == MAIN_PROFILER) {
        return "Profiler";
    } else if (i < MAIN_PAGES + sPageCount) {
        return sPages[i - MAIN_PAGES].title;
    }
    return sLast[i - MAIN_PAGES - sPageCount];
}

static void row_value(int i, char *out, int size) {
    if (i == MAIN_AA) {
        snprintf(out, size, "%s", sAaNames[plat_3ds_antialias_setting()]);
    } else if (i == MAIN_FPS) {
        snprintf(out, size, "%s", plat_60fps_setting() ? "60 fps" : "30 fps");
    } else if (i == MAIN_FB16) {
        snprintf(out, size, "%s", plat_3ds_fb16_setting() ? "On" : "Off");
    } else if (i == MAIN_MIPMAPS) {
        snprintf(out, size, "%s", sMipmapModes[plat_3ds_mipmap_setting()]);
    } else if (i == MAIN_COLOUR) {
        snprintf(out, size, "%s", sColourLevels[plat_3ds_colour_setting()]);
    } else if (i == MAIN_AHEAD) {
        snprintf(out, size, "%d frame%s", plat_render_ahead(), plat_render_ahead() == 1 ? "" : "s");
    } else if (i == MAIN_PROFILER) {
        snprintf(out, size, "%s", prof_user_enabled() ? "On" : "Off");
    } else {
        out[0] = '\0';
    }
}

static const char *row_hint(int i) {
    static const char *const sLast[3] = { NULL, "Install a newer build from GitHub",
                                          "Log and snapshot (also SELECT+touch)" };
    if (i == MAIN_AA) {
        return plat_3ds_antialias_setting() == PLAT_AA_2X ? "Smoother edges; the GPU draws 2x"
                                                           : "Smoother edges; the GPU draws 4x";
    } else if (i == MAIN_FPS) {
        return "60 when a frame fits (try AA off)";
    } else if (i == MAIN_FB16) {
        return "Less GPU memory work, like the N64";
    } else if (i == MAIN_MIPMAPS) {
        return plat_3ds_mipmap_setting() == 2 ? "Tints each level a colour" : "No shimmer on distant textures";
    } else if (i == MAIN_COLOUR) {
        return "Richer colours on the 3DS screen";
    } else if (i == MAIN_SCREEN) {
        return "Widescreen, 800 pixels, 3D";
    } else if (i == MAIN_AHEAD) {
        return plat_render_ahead() == 2 ? "Fewer dropped frames, a frame more lag" : "The N64's timing";
    } else if (i == MAIN_PROFILER) {
        return "Frame times on this screen";
    } else if (i < MAIN_PAGES + sPageCount) {
        return NULL;
    } else if (i == MAIN_PAGES + sPageCount) {
        return port_autorun_enabled() ? "Running now: open to stop it" : "AI races that look for problems";
    }
    return sLast[i - MAIN_PAGES - sPageCount];
}

static void row_change(int i, int step) {
    if (i == MAIN_AA) {
        int at;
        for (at = 0; at < 3 && sAaOrder[at] != plat_3ds_antialias_setting(); at++) {}
        plat_3ds_set_antialias_setting(sAaOrder[(at + step + 3) % 3]);
    } else if (i == MAIN_FPS) {
        plat_set_60fps_setting(!plat_60fps_setting());
    } else if (i == MAIN_FB16) {
        plat_3ds_set_fb16_setting(!plat_3ds_fb16_setting());
    } else if (i == MAIN_MIPMAPS) {
        int modes = PLAT_DEBUG_TOOLS ? 3 : 2; // Show is for debugging.
        plat_3ds_set_mipmap_setting((plat_3ds_mipmap_setting() + step + modes) % modes);
    } else if (i == MAIN_COLOUR) {
        plat_3ds_set_colour_setting((plat_3ds_colour_setting() + step + 4) % 4);
    } else if (i == MAIN_SCREEN) {
        open_page(&sScreenPage);
    } else if (i == MAIN_AHEAD) {
        plat_set_render_ahead(plat_render_ahead() == 1 ? 2 : 1);
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

static int main_count(void) {
    return options_visible_count(main_row_count(), main_hidden);
}

static const char *main_label(int i) {
    return row_label(options_visible_row(main_row_count(), main_hidden, i));
}

static void main_value(int i, char *out, int size) {
    row_value(options_visible_row(main_row_count(), main_hidden, i), out, size);
}

static const char *main_hint(int i) {
    return row_hint(options_visible_row(main_row_count(), main_hidden, i));
}

static void main_change(int i, int step) {
    row_change(options_visible_row(main_row_count(), main_hidden, i), step);
}

static const MenuPage sMainPage = { "Settings", main_count, main_label, main_value, main_change, main_hint, NULL };

/* The top screen (roadmap phase 6, gfx_c3d.c): widescreen, the 800 pixel mode and stereo 3D, each on or off, where
 * widescreen puts the race HUD (plat_hud_edges), and edge trimming (the guard band clip, a test toggle for the blue
 * patches in widescreen, issue #147, which a player's build leaves out). As on the main page, the screen_row_* functions
 * number every row. */
enum { SCREEN_HUD = 3, SCREEN_GUARD_BAND = 4, SCREEN_ROWS = 5 };

static int screen_hidden(int row) {
    return !PLAT_DEBUG_TOOLS && row == SCREEN_GUARD_BAND;
}

static const char *screen_row_label(int i) {
    static const char *const sLabels[5] = { "Widescreen", "800 pixels", "Stereo 3D", "HUD", "Edge trimming" };
    return sLabels[i];
}

static const char *const sWideStyles[4] = { "Off", "Wide", "Balanced", "Zoomed" };

static void screen_row_value(int i, char *out, int size) {
    if (i == 0) {
        snprintf(out, size, "%s", sWideStyles[plat_3ds_screen_setting(0)]);
    } else if (i == SCREEN_HUD) {
        snprintf(out, size, "%s", plat_hud_edges() ? "Screen edges" : "4:3");
    } else if (i == SCREEN_GUARD_BAND) {
        snprintf(out, size, "%s", plat_3ds_guard_band_setting() ? "On" : "Off");
    } else {
        snprintf(out, size, "%s", plat_3ds_screen_setting(i) ? "On" : "Off");
    }
}

static const char *screen_row_hint(int i) {
    if (i == 0) {
        static const char *const sHints[4] = { "4:3 picture in the middle", "Most to the sides; edges stretch most",
                                               "Between Wide and Zoomed", "4:3's sides, less top; no extra stretch" };
        return sHints[plat_3ds_screen_setting(0)];
    } else if (i == 1) {
        return plat_3ds_wide800_supported() ? "Sharper picture while 3D is off" : "Not on this console (2DS)";
    } else if (i == SCREEN_HUD) {
        return plat_hud_edges() ? "Widescreen HUD out at the sides" : "Widescreen HUD in the 4:3 middle";
    } else if (i == SCREEN_GUARD_BAND) {
        return "Cuts triangles far off screen";
    }
    return "With the 3D slider; no anti-aliasing in 3D";
}

static void screen_row_change(int i, int step) {
    if (i == 0) {
        plat_3ds_set_screen_setting(0, (plat_3ds_screen_setting(0) + step + 4) % 4);
    } else if (i == SCREEN_HUD) {
        plat_set_hud_edges(!plat_hud_edges());
    } else if (i == SCREEN_GUARD_BAND) {
        plat_3ds_set_guard_band_setting(!plat_3ds_guard_band_setting());
    } else {
        plat_3ds_set_screen_setting(i, !plat_3ds_screen_setting(i));
    }
}

static int screen_count(void) {
    return options_visible_count(SCREEN_ROWS, screen_hidden);
}

static const char *screen_label(int i) {
    return screen_row_label(options_visible_row(SCREEN_ROWS, screen_hidden, i));
}

static void screen_value(int i, char *out, int size) {
    screen_row_value(options_visible_row(SCREEN_ROWS, screen_hidden, i), out, size);
}

static const char *screen_hint(int i) {
    return screen_row_hint(options_visible_row(SCREEN_ROWS, screen_hidden, i));
}

static void screen_change(int i, int step) {
    screen_row_change(options_visible_row(SCREEN_ROWS, screen_hidden, i), step);
}

static const MenuPage sScreenPage = {
    "Screen", screen_count, screen_label, screen_value, screen_change, screen_hint, "Changes show from the next frame"
};

/* The test run (port_autorun.c): stop the one running, or start one of its five kinds. */
static int test_count(void) {
    return port_autorun_enabled() ? 1 : 5;
}

static const char *test_label(int i) {
    static const char *const sStart[5] = { "Full test", "Quick test", "Boss fights", "GPU bench", "Switch bench" };
    return port_autorun_enabled() ? "Stop the test run" : sStart[i];
}

static void test_value(int i, char *out, int size) {
    (void) i;
    (void) size;
    out[0] = '\0';
}

static const char *test_hint(int i) {
    static const char *const sStart[5] = { "Every track and character, ~8 hours", "5 slowest tracks twice, ~20 minutes",
                                           "All 10 boss races once, ~30 minutes", "3 heavy tracks x 8 GPU settings, ~50 min",
                                           NULL };
    static char sSwitchHint[48];
    if (port_autorun_enabled()) {
        return "The results so far go out as a report";
    }
    if (i == 4) {
        long sets = port_autorun_switch_count() + 1; // The usual settings, then each switch flipped
        if (sets < 2) {
            return "No switches to test right now";
        }
        snprintf(sSwitchHint, sizeof(sSwitchHint), "3 heavy tracks x %ld switch sets, ~%ld min", sets,
                 (sets * 13 + 1) / 2);
        return sSwitchHint;
    }
    return sStart[i];
}

static void test_change(int i, int step) {
    (void) step;
    if (port_autorun_enabled()) {
        port_autorun_request(0);
    } else {
        // port_autorun_request: 1 the full run, 3 the quick test, 4 the boss fights, 2 the GPU benchmark, 5 the switch
        // bench.
        static const long sRequests[5] = { 1, 3, 4, 2, 5 };
        port_autorun_request(sRequests[i]);
    }
    // The run turns on or off at the game's next frame.
    svcSleepThread(200 * 1000 * 1000);
    sResult = MENU_CLOSE;
}

static const MenuPage sTestRunPage = {
    "Test run", test_count, test_label, test_value, test_change, test_hint, "Starts at the title; saves not written"
};

/*
 * The game's VIDEO OPTIONS (plat/options.h): the main page's anti-aliasing to colour boost rows, then the Screen
 * page's, then Render ahead, less the ones a player's build hides. Every one can change mid-race, as here.
 */
enum { VIDEO_ROWS = MAIN_SCREEN + SCREEN_ROWS + 1 };

/* The main page row a Video Options row (of all of them) stands for, or -1 for a Screen page row. */
static int video_main_row(int row) {
    return row < MAIN_SCREEN ? row : row == MAIN_SCREEN + SCREEN_ROWS ? MAIN_AHEAD : -1;
}

static int video_hidden(int row) {
    int main = video_main_row(row);
    return main >= 0 ? main_hidden(main) : screen_hidden(row - MAIN_SCREEN);
}

static int video_count(void) {
    return options_visible_count(VIDEO_ROWS, video_hidden);
}

static const char *video_label(int i) {
    int row = options_visible_row(VIDEO_ROWS, video_hidden, i), main = video_main_row(row);
    return main >= 0 ? row_label(main) : screen_row_label(row - MAIN_SCREEN);
}

static void video_value(int i, char *out, int size) {
    int row = options_visible_row(VIDEO_ROWS, video_hidden, i), main = video_main_row(row);
    if (main >= 0) {
        row_value(main, out, size);
    } else {
        screen_row_value(row - MAIN_SCREEN, out, size);
    }
}

static void video_change(int i, int step) {
    int row = options_visible_row(VIDEO_ROWS, video_hidden, i), main = video_main_row(row);
    if (main >= 0) {
        row_change(main, step);
    } else {
        screen_row_change(row - MAIN_SCREEN, step);
    }
}

static const char *video_hint(int i) {
    int row = options_visible_row(VIDEO_ROWS, video_hidden, i), main = video_main_row(row);
    return main >= 0 ? row_hint(main) : screen_row_hint(row - MAIN_SCREEN);
}

void menu_3ds_add_options(void) {
    static const OptionsSection sVideo = { video_count, video_label, video_value, video_change, video_hint, NULL };
    options_add(OPTIONS_VIDEO, &sVideo);
}

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
    sDrawnChanges = options_change_count();
}

/* The panel's calls. */

void menu_3ds_open(void) {
    open_page(&sMainPage);
}

/* Opens the page added with this title (the panel's Controls button). Returns 0 if there is none. */
int menu_3ds_open_page(const char *title) {
    int i;
    for (i = 0; i < sPageCount; i++) {
        if (strcmp(sPages[i].title, title) == 0) {
            open_page(&sPages[i]);
            return 1;
        }
    }
    return 0;
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

/* Every few seconds: the test run can end by itself, and the game's menus can change a setting shown. */
void menu_3ds_refresh(void) {
    if (sPage != NULL &&
        (sDrawnTestRun != (port_autorun_enabled() != 0) || sDrawnChanges != options_change_count())) {
        draw();
    }
}
