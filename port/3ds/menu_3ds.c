/*
 * The Settings menu on the bottom screen. The panel's "Settings" button (update_3ds.c) opens it over the scrolling
 * area (plat_3ds_menu_show). A page is a list of rows, each a label with its value between "<" and ">" (or just ">"
 * for a row that does something or opens a page), and a hint line under it. Tapping the "<" half of a value steps it
 * back, anywhere else on the row steps it forward. The panel shows Close (Back on another page), and Prev / Next when
 * a page has more rows than fit.
 *
 * Other parts of the port add a page with menu_3ds_add_page; the main page gets a row that opens it. Settings are
 * saved in settings.txt (plat/settings.h) or a file of their owner's. Everything here runs on the bottom screen's
 * thread (update_3ds.c), so a page's functions are called from that thread.
 */
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "menu_3ds.h"
#include "plat/plat.h"

void plat_3ds_menu_print(int row, const char *text);
void plat_3ds_menu_show(int show);
void plat_3ds_set_antialias_setting(int on);
int plat_3ds_antialias_setting(void);
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

/* Rows 3-23 hold 7 rows of the page, 3 screen rows each (label, hint, gap); row 24 the note, 25 the newest message. */
#define FIRST_ROW 3
#define ROWS_PER_PAGE 7
#define NOTE_ROW 24
/* The value field: columns 24-39 ("< value >"); a tap on columns 24-31 steps back. */
#define VALUE_COLUMN 24

#define MAX_PAGES 8
static MenuPage sPages[MAX_PAGES];
static int sPageCount;

static const MenuPage *sPage;
static int sFirst;  // First row shown.
static int sResult; // What a tap asked of the panel (MENU_*).
static int sDrawnTestRun = -1;

static const MenuPage sMainPage, sTestRunPage;

static void draw(void);

static void open_page(const MenuPage *page) {
    sPage = page;
    sFirst = 0;
    draw();
}

/* The main page: anti-aliasing, the profiler, the added pages, then the test run, updates and bug report. */
enum { MAIN_AA, MAIN_PROFILER, MAIN_PAGES };

static int main_count(void) {
    return MAIN_PAGES + sPageCount + 3;
}

static const char *main_label(int i) {
    static const char *const sLast[3] = { "Test run", "Check for updates", "Send a bug report" };
    if (i == MAIN_AA) {
        return "Anti-aliasing";
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
    (void) step;
    if (i == MAIN_AA) {
        plat_3ds_set_antialias_setting(!plat_3ds_antialias_setting());
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

/* The test run (port_autorun.c): stop the one running, or start one of its three kinds. */
static int test_count(void) {
    return port_autorun_enabled() ? 1 : 3;
}

static const char *test_label(int i) {
    static const char *const sStart[3] = { "Full test", "Quick test", "AA bench" };
    return port_autorun_enabled() ? "Stop the test run" : sStart[i];
}

static void test_value(int i, char *out, int size) {
    (void) i;
    (void) size;
    out[0] = '\0';
}

static const char *test_hint(int i) {
    static const char *const sStart[3] = { "Every track and character, ~8 hours", "5 slowest tracks twice, ~20 minutes",
                                           "Heavy races, AA off and on, ~30 min" };
    return port_autorun_enabled() ? "The results so far go out as a report" : sStart[i];
}

static void test_change(int i, int step) {
    (void) step;
    if (port_autorun_enabled()) {
        port_autorun_request(0);
    } else {
        // port_autorun_request: 1 the full run, 3 the quick test, 2 the anti-aliasing benchmark.
        static const long sRequests[3] = { 1, 3, 2 };
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

static void draw_row(int slot) {
    int i = sFirst + slot, row = FIRST_ROW + slot * 3;
    char line[160], value[48], field[24];
    const char *hint;

    if (i >= sPage->count()) {
        plat_3ds_menu_print(row, "");
        plat_3ds_menu_print(row + 1, "");
        return;
    }
    sPage->value(i, value, sizeof(value));
    if (value[0] != '\0') {
        int width = 12, length = (int) strlen(value) > width ? width : (int) strlen(value);
        int left = (width - length) / 2;
        snprintf(field, sizeof(field), "< %*s%.*s%*s >", left, "", length, value, width - length - left, "");
    } else {
        snprintf(field, sizeof(field), "%15s", ">");
    }
    // 39 columns: a space, the label (22), then the value field (16).
    snprintf(line, sizeof(line), "\x1b[37;44m %-22.22s%-16s", sPage->label(i), field);
    plat_3ds_menu_print(row, line);
    hint = sPage->hint != NULL ? sPage->hint(i) : NULL;
    snprintf(line, sizeof(line), "\x1b[36;44m   %-36.36s", hint != NULL ? hint : "");
    plat_3ds_menu_print(row + 1, line);
}

static void draw(void) {
    int count = sPage->count(), slot;
    char line[80];

    if (sFirst >= count) {
        sFirst = 0;
    }
    if (count > ROWS_PER_PAGE) {
        char pages[24];
        snprintf(pages, sizeof(pages), "%d/%d", sFirst / ROWS_PER_PAGE + 1,
                 (count + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE);
        snprintf(line, sizeof(line), "\x1b[33;1m %-32.32s%6s", sPage->title, pages);
    } else {
        snprintf(line, sizeof(line), "\x1b[33;1m %s", sPage->title);
    }
    plat_3ds_menu_print(1, line);
    plat_3ds_menu_print(2, "");
    for (slot = 0; slot < ROWS_PER_PAGE; slot++) {
        draw_row(slot);
        plat_3ds_menu_print(FIRST_ROW + slot * 3 + 2, "");
    }
    if (sPage->note != NULL) {
        snprintf(line, sizeof(line), "\x1b[37m %s", sPage->note);
    } else {
        snprintf(line, sizeof(line), "\x1b[37m Build %.32s", plat_build_version());
    }
    plat_3ds_menu_print(NOTE_ROW, line);
    sDrawnTestRun = port_autorun_enabled() != 0;
}

/* The panel's calls. */

void menu_3ds_open(void) {
    plat_3ds_menu_show(1);
    open_page(&sMainPage);
}

void menu_3ds_close(void) {
    plat_3ds_menu_show(0);
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
    int row = y / 8 + 1, column = x / 8 + 1, slot, i;
    char value[48];

    if (sPage == NULL || row < FIRST_ROW || (row - FIRST_ROW) % 3 == 2) {
        return MENU_STAY;
    }
    slot = (row - FIRST_ROW) / 3;
    i = sFirst + slot;
    if (slot >= ROWS_PER_PAGE || i >= sPage->count()) {
        return MENU_STAY;
    }
    sPage->value(i, value, sizeof(value));
    sResult = MENU_STAY;
    sPage->change(i, value[0] != '\0' && column >= VALUE_COLUMN && column < VALUE_COLUMN + 8 ? -1 : 1);
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
