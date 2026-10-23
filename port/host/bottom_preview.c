/*
 * DKR_BOTTOM_PREVIEW=dir: draws the 3DS bottom screen's views (src/plat/bottom_screen.c) with the ROM's fonts and
 * pictures to PNG files, to see them without a 3DS. DKR_MONO_FONT can name a file with the 8x8 font the 3DS uses for
 * dense text (libctru's, 2048 bytes); without it the small font stands in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat/bottom_screen.h"

int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes);

static unsigned short sImage[BS_WIDTH * BS_HEIGHT];
static unsigned char sRgba[BS_WIDTH * BS_HEIGHT * 4];

static void save(const char *dir, const char *name) {
    char path[512];
    int area, i;
    bs_take_changes();
    for (area = 0; area < BS_AREA_COUNT; area++) {
        bs_draw(sImage, area);
    }
    for (i = 0; i < BS_WIDTH * BS_HEIGHT; i++) {
        unsigned short p = sImage[i];
        sRgba[i * 4] = (p >> 11) << 3;
        sRgba[i * 4 + 1] = ((p >> 5) & 63) << 2;
        sRgba[i * 4 + 2] = (p & 31) << 3;
        sRgba[i * 4 + 3] = 255;
    }
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    stbi_write_png(path, BS_WIDTH, BS_HEIGHT, 4, sRgba, BS_WIDTH * 4);
    printf("wrote %s\n", path);
}

static void row(BsMenu *m, const char *label, const char *value, const char *hint) {
    BsMenuRow *r = &m->rows[m->rowCount++];
    snprintf(r->label, sizeof(r->label), "%s", label);
    snprintf(r->value, sizeof(r->value), "%s", value);
    snprintf(r->hint, sizeof(r->hint), "%s", hint);
}

void host_bottom_preview(const char *dir) {
    static unsigned char sMono[2048];
    const char *monoPath = getenv("DKR_MONO_FONT");
    const unsigned char *mono = NULL;
    BsMenu menu;

    if (monoPath != NULL) {
        FILE *f = fopen(monoPath, "rb");
        if (f != NULL && fread(sMono, 1, sizeof(sMono), f) == sizeof(sMono)) {
            mono = sMono;
        }
        if (f != NULL) {
            fclose(f);
        }
    }
    bs_init(mono);
    {
        static const char *const sSettings[1] = { "Settings" };
        bs_set_buttons(sSettings, 1);
    }
    bs_set_status_line(0, " 42s vi +60 gfx 1260/1260 aud +60 sys+62");
    bs_set_status_line(1, "gap 0 drop 0 tex 0 gpu 12.3M heap 20.1M");
    bs_add_message("New 3DS, renderer on core 2");
    bs_add_message("Loading the ROM...");
    bs_add_message("ROM loaded in 1.2 s");
    save(dir, "0_before_rom");

    bs_load_assets();
    bs_add_message("Starting the game...");
    bs_add_message("Saves and settings: sdmc:/3ds/dkr/");
    save(dir, "1_home");

    bs_set_overlay("PROFILER 29.8 fps  limit: gpu\n"
                   "30fps  97%  20fps   3%  slower   0%\n"
                   "worst frame  52 ms  hitches 1\n"
                   "game  logic  6.1 scene  9.8 wait 12.0\n"
                   "rend  rdp  8.2 tex  0.4 gpuwait  3.1\n"
                   "gpu   draw 21.7  audio  2.0  idle 18%\n"
                   "tris  3120 draws  412 tex+ 3\n");
    {
        static const char *const sRunning[2] = { "Settings", "Stop test" };
        bs_set_buttons(sRunning, 2);
    }
    save(dir, "2_profiler_test_run");
    bs_set_overlay("");

    memset(&menu, 0, sizeof(menu));
    snprintf(menu.title, sizeof(menu.title), "Settings");
    row(&menu, "Anti-aliasing", "On", "Smoother edges; the GPU draws 4x");
    row(&menu, "Profiler", "Off", "Frame times on this screen");
    row(&menu, "Controls", "", "");
    row(&menu, "Test run", "", "AI races that look for problems");
    row(&menu, "Check for updates", "", "Install a newer build from GitHub");
    row(&menu, "Send a bug report", "", "Log and snapshot (also SELECT+touch)");
    snprintf(menu.note, sizeof(menu.note), "Build 1a2b3c4");
    bs_show_menu(&menu);
    {
        static const char *const sMenuButtons[3] = { "Close", "-Prev", "-Next" };
        bs_set_buttons(sMenuButtons, 3);
    }
    save(dir, "3_menu");

    memset(&menu, 0, sizeof(menu));
    snprintf(menu.title, sizeof(menu.title), "Controls");
    snprintf(menu.pages, sizeof(menu.pages), "1/3");
    row(&menu, "Layout", "Standard", "");
    row(&menu, "A button", "A", "");
    row(&menu, "B button", "B", "");
    row(&menu, "Deadzone", "6%", "");
    row(&menu, "Sensitivity", "100%", "");
    row(&menu, "Response curve", "Linear", "");
    menu.note[0] = '\0';
    bs_show_menu(&menu);
    {
        static const char *const sMenuButtons[3] = { "Back", "-Prev", "Next" };
        bs_set_buttons(sMenuButtons, 3);
    }
    save(dir, "4_controls");

    bs_show_menu(NULL);
    bs_add_message("This build: 1a2b3c4");
    bs_add_message("master 92709d8 master 2026-10-23");
    bs_add_message("newest 5f2a9a2 claude/project-thread 2026-10-23 (this)");
    {
        static const char *const sChoices[3] = { "master", "-newest", "cancel" };
        bs_set_buttons(sChoices, 3);
    }
    save(dir, "5_update_choices");
    bs_set_panel_text("Downloading master... 45%");
    save(dir, "6_busy");
    {
        static const char *const sConfirm[3] = { "Stop test", "-", "cancel" };
        bs_set_buttons(sConfirm, 3);
    }
    bs_add_message("Stop the test run? The results so far are sent as a report.");
    save(dir, "7_confirm");
}
