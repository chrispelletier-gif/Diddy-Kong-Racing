/*
 * DKR_BOTTOM_PREVIEW=dir: draws the 3DS bottom screen's views (src/plat/bottom_screen.c) with the ROM's fonts and
 * pictures to PNG files, to see them without a 3DS. DKR_MONO_FONT can name a file with the 8x8 font the 3DS uses for
 * dense text (libctru's, 2048 bytes); without it the small font stands in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat/bottom_screen.h"
#include "plat/plat.h"

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

static void preview_race(const char *dir);
static void preview_modes(const char *dir, RaceHud *race);

static void row(BsMenu *m, const char *label, const char *value, const char *hint) {
    BsMenuRow *r = &m->rows[m->rowCount++];
    snprintf(r->label, sizeof(r->label), "%s", label);
    snprintf(r->value, sizeof(r->value), "%s", value);
    snprintf(r->hint, sizeof(r->hint), "%s", hint);
}

/* The menu's tabs, as a player's build (count 8) or a debug build (10) has them, with tab picked. */
static void tabs(BsMenu *m, int picked, int count) {
    static const char *const sTabs[10] = { "Picture", "Screen",   "Controls", "Sound",    "Race HUD",
                                           "Camera",  "Start-up", "Saves",    "Test run", "Tools" };
    int i;
    for (i = 0; i < count; i++) {
        snprintf(m->tabs[i], sizeof(m->tabs[i]), "%s", sTabs[i]);
    }
    m->tabCount = count;
    m->tab = picked;
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
    if (getenv("DKR_BOTTOM_STYLE") != NULL) {
        bs_set_style(atoi(getenv("DKR_BOTTOM_STYLE")));
    }
    {
        static const char *const sHome[3] = { "Settings", "Controls", "Updates" };
        bs_set_buttons(sHome, 3);
    }
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

    // The Settings menu: a player's tabs, then a debug build's.
    memset(&menu, 0, sizeof(menu));
    tabs(&menu, 0, 8);
    snprintf(menu.title, sizeof(menu.title), "Picture");
    row(&menu, "Anti-aliasing", "4x", "Smoother edges; the GPU draws 4x");
    row(&menu, "Frame rate", "30 fps", "60 when a frame fits (try AA off)");
    row(&menu, "Mipmaps", "On", "No shimmer on distant textures");
    row(&menu, "Colour boost", "Medium", "Richer colours on the 3DS screen");
    menu.picked = 0;
    snprintf(menu.note, sizeof(menu.note), "Tap a value's arrows to change it");
    bs_show_menu(&menu);
    {
        static const char *const sMenuButtons[3] = { "Close", "-Prev", "-Next" };
        bs_set_buttons(sMenuButtons, 3);
    }
    save(dir, "3_menu");

    memset(&menu, 0, sizeof(menu));
    tabs(&menu, 2, 10);
    snprintf(menu.title, sizeof(menu.title), "Controls");
    snprintf(menu.pages, sizeof(menu.pages), "1/3");
    row(&menu, "Layout", "Standard", "");
    row(&menu, "A button", "A", "");
    row(&menu, "B button", "B", "");
    row(&menu, "Deadzone", "6%", "");
    row(&menu, "Response curve", "Linear", "");
    menu.picked = 3;
    snprintf(menu.note, sizeof(menu.note), "Tap a value's arrows to change it");
    bs_show_menu(&menu);
    {
        static const char *const sMenuButtons[3] = { "Close", "-Prev", "Next" };
        bs_set_buttons(sMenuButtons, 3);
    }
    save(dir, "4_controls");

    memset(&menu, 0, sizeof(menu));
    tabs(&menu, 9, 10);
    snprintf(menu.title, sizeof(menu.title), "Tools");
    row(&menu, "Profiler", "Off", "Frame times on this screen");
    row(&menu, "Check for updates", "", "Install a newer build from GitHub");
    row(&menu, "Send a bug report", "", "Log and snapshot (also SELECT+touch)");
    menu.picked = -1;
    snprintf(menu.note, sizeof(menu.note), port_text("Build %.32s"), "1a2b3c4");
    bs_show_menu(&menu);
    {
        static const char *const sMenuButtons[3] = { "Close", "-Prev", "-Next" };
        bs_set_buttons(sMenuButtons, 3);
    }
    save(dir, "4_tools");

    memset(&menu, 0, sizeof(menu));
    tabs(&menu, 7, 10);
    snprintf(menu.title, sizeof(menu.title), "Saves");
    row(&menu, "Back up now", "", "Saves a dated copy, ghosts included");
    row(&menu, "Backup", "10-26 23:45", "1 of 3 in sdmc:/3ds/dkr/backup");
    row(&menu, "Tap again to restore", "", "Overwrites your save, then quits");
    menu.picked = 2;
    snprintf(menu.note, sizeof(menu.note), "Saved as N64 dkr.eep and dkr.mpk");
    bs_show_menu(&menu);
    {
        static const char *const sMenuButtons[3] = { "Close", "-Prev", "-Next" };
        bs_set_buttons(sMenuButtons, 3);
    }
    save(dir, "4_saves");
    menu.picked = -1;
    tabs(&menu, 7, 8);
    snprintf(menu.rows[2].label, sizeof(menu.rows[2].label), "Restore backup");
    bs_show_menu(&menu);
    save(dir, "4_saves_player");

    bs_show_menu(NULL);
    {
        // As update_3ds.c's plat_status lines show them (in the game's language).
        char line[96];
        snprintf(line, sizeof(line), port_text("This build: %.7s"), "1a2b3c4");
        bs_add_message(line);
        bs_add_message("master 92709d8 master 2026-10-23");
        snprintf(line, sizeof(line), "newest 5f2a9a2 claude/project-thread 2026-10-23%s", port_text(" (this)"));
        bs_add_message(line);
    }
    {
        static const char *const sChoices[3] = { "master", "-newest", "cancel" };
        bs_set_buttons(sChoices, 3);
    }
    save(dir, "5_update_choices");
    {
        char line[64];
        snprintf(line, sizeof(line), port_text("Downloading %s: %d%%"), "master", 45);
        bs_set_panel_text(line);
    }
    save(dir, "6_busy");
    {
        static const char *const sConfirm[3] = { "Stop test", "-", "cancel" };
        bs_set_buttons(sConfirm, 3);
    }
    bs_add_message("Stop the test run? The results so far are sent as a report.");
    save(dir, "7_confirm");
    bs_show_menu(NULL);
    preview_race(dir);
}

/* A race as the mockups showed it ("standings tower"), for the preview. */
static void preview_race(const char *dir) {
    static const signed char sOrder[8] = { 0, 5, 9, 3, 2, 4, 7, 1 }; // Krunch, Banjo, Diddy (the player), Conker...
    static const int sGaps[8] = { 85, 23, 0, -37, -114, -183, -286, -367 };
    static const char *const sButtons[2] = { "Pause", "Settings" };
    RaceHud hud;
    int i;
    memset(&hud, 0, sizeof(hud));
    hud.mapSprite = -1;
    hud.silverCoins = -1;
    hud.racerCount = 8;
    for (i = 0; i < 8; i++) {
        hud.racers[i].character = sOrder[i];
        hud.racers[i].special = RACE_HUD_NOT_SPECIAL;
        hud.racers[i].player = i == 2;
        hud.racers[i].gap = i == 2 ? RACE_HUD_NO_GAP : sGaps[i];
    }
    hud.position = 3;
    hud.lap = 2;
    hud.laps = 3;
    hud.lapTimes[0] = 1861;
    hud.lapTimes[1] = 1879;
    hud.lapsTimed = 2;
    hud.raceTime = 1861 + 1879;
    hud.item = 1;
    hud.itemLevel = 1;
    hud.itemCount = 3;
    hud.bananas = 7;
    bs_set_buttons(sButtons, 2);
    bs_set_race_hud(&hud);
    save(dir, "8_race");
    hud.lap = 3;
    hud.item = -1;
    hud.position = 1;
    bs_set_race_hud(&hud);
    save(dir, "9_race_final_lap");
    hud.lap = 2;
    hud.position = 3;
    hud.silverCoins = 5;
    bs_set_race_hud(&hud);
    save(dir, "10_silver_coins");
    preview_modes(dir, &hud);
    bs_set_race_hud(NULL);
}

static void set_racer(RaceHudRacer *r, int character, int special, int player, int gap) {
    memset(r, 0, sizeof(*r));
    r->character = character;
    r->special = special;
    r->player = player;
    r->gap = gap;
}

/* The other modes, as the mockups in 3ds/bottom-ui/mode-mockups showed them. */
static void preview_modes(const char *dir, RaceHud *race) {
    static const signed char sChallengers[4] = { 0, 9, 7, 4 }; // Krunch, Diddy (the player), Pipsy, Timber
    static const signed char sScores[4] = { 7, 9, 4, 2 };
    RaceHud hud;
    int i;

    hud = *race;
    hud.mode = RACE_HUD_MODE_TIME_TRIAL;
    hud.silverCoins = -1;
    hud.racerCount = 1;
    set_racer(&hud.racers[0], 9, RACE_HUD_NOT_SPECIAL, 1, RACE_HUD_NO_GAP);
    hud.item = -1;
    hud.record = 5530;
    hud.recordLap = 1812;
    hud.staffTime = 5490;
    hud.bestGap = -42;
    hud.bestLaps[0] = 1890;
    hud.bestLaps[1] = 1870;
    hud.bestLaps[2] = 1850;
    hud.bestLapCount = 3;
    hud.bestTotal = 5610;
    bs_set_race_hud(&hud);
    save(dir, "11_time_trial");

    hud = *race;
    hud.mode = RACE_HUD_MODE_BOSS;
    hud.silverCoins = -1;
    hud.round = 2;
    hud.racerCount = 2;
    set_racer(&hud.racers[0], 9, RACE_HUD_NOT_SPECIAL, 1, RACE_HUD_NO_GAP);
    set_racer(&hud.racers[1], 0, RACE_HUD_TRICKY, 0, -95);
    hud.position = 1;
    bs_set_race_hud(&hud);
    save(dir, "12_boss");

    hud.mode = RACE_HUD_MODE_TAJ;
    hud.vehicle = 2;
    set_racer(&hud.racers[0], 0, RACE_HUD_TAJ, 0, 140);
    set_racer(&hud.racers[1], 9, RACE_HUD_NOT_SPECIAL, 1, RACE_HUD_NO_GAP);
    hud.position = 2;
    bs_set_race_hud(&hud);
    save(dir, "13_taj");

    hud = *race;
    hud.silverCoins = -1;
    hud.laps = 0;
    hud.lapsTimed = 0;
    hud.racerCount = 4;
    for (i = 0; i < 4; i++) {
        set_racer(&hud.racers[i], sChallengers[i], RACE_HUD_NOT_SPECIAL, i == 1, RACE_HUD_NO_GAP);
        hud.racers[i].score = sScores[i];
        hud.racers[i].carrying = i == 1;
        hud.racers[i].character = sChallengers[i];
    }
    hud.mode = RACE_HUD_MODE_BANANAS;
    hud.goal = 10;
    bs_set_race_hud(&hud);
    save(dir, "14_bananas");
    hud.mode = RACE_HUD_MODE_EGGS;
    hud.goal = 3;
    for (i = 0; i < 4; i++) {
        hud.racers[i].score = sScores[i] % 3;
    }
    bs_set_race_hud(&hud);
    save(dir, "15_eggs");
    hud.mode = RACE_HUD_MODE_BATTLE;
    hud.goal = 0;
    for (i = 0; i < 4; i++) {
        hud.racers[i].score = 3 - i;
        hud.racers[i].out = i == 3;
    }
    hud.racersLeft = 3;
    bs_set_race_hud(&hud);
    save(dir, "16_battle");

    memset(&hud, 0, sizeof(hud));
    hud.mode = RACE_HUD_MODE_HUB;
    hud.mapSprite = -1;
    hud.world = 0;
    hud.balloons = 23;
    for (i = 0; i < RACE_HUD_WORLDS; i++) {
        hud.worldBalloons[i] = i < 2 ? 8 - i : i == 2 ? 3 : 0;
        hud.worldTrophy[i] = i == 0 ? 3 : i == 1 ? 1 : 0;
    }
    hud.worldKeys = 3;
    hud.worldBosses = 1;
    hud.ttAmulet = 2;
    hud.wizpigAmulet = 1;
    hud.racerCount = 1;
    set_racer(&hud.racers[0], 9, RACE_HUD_NOT_SPECIAL, 1, RACE_HUD_NO_GAP);
    bs_set_race_hud(&hud);
    save(dir, "17_hub");

    // The menus' companions, with the idle buttons.
    {
        static const char *const sHome[3] = { "Settings", "Controls", "Updates" };
        bs_set_buttons(sHome, 3);
    }
    hud.mode = RACE_HUD_MODE_MENU_SAVE;
    hud.world = -1;
    hud.racerCount = 0;
    hud.worldBosses = 3;
    snprintf(hud.name, sizeof(hud.name), "DKR");
    bs_set_race_hud(&hud);
    save(dir, "18_menu_save");
    memset(&hud, 0, sizeof(hud));
    hud.mode = RACE_HUD_MODE_MENU_TRACK;
    hud.mapSprite = -1;
    hud.level = 10; // Crescent Island.
    hud.record = 5500;
    hud.recordLap = 1790;
    hud.bestTotal = 5610;
    snprintf(hud.name, sizeof(hud.name), "Crescent Island");
    bs_set_race_hud(&hud);
    save(dir, "19_menu_track");
}

static int sRaceFrames;

void host_bottom_race_start(void) {
    static const char *const sButtons[2] = { "Pause", "Settings" };
    bs_init(NULL);
    bs_load_assets();
    if (getenv("DKR_BOTTOM_STYLE") != NULL) {
        bs_set_style(atoi(getenv("DKR_BOTTOM_STYLE")));
    }
    bs_set_buttons(sButtons, 2);
}

/* DKR_BOTTOM_RACE: the race HUD every 60 game frames, as race_NNNN.png. */
void host_bottom_race(const char *dir, const RaceHud *hud) {
    char name[32];
    if (getenv("DKR_BOTTOM_RACE_LOG") != NULL) {
        // The player's map position and heading each frame, to check them against the track's map.
        int i;
        for (i = 0; i < hud->racerCount; i++) {
            if (hud->racers[i].player) {
                printf("player %d %d %u\n", hud->racers[i].at.mapX, hud->racers[i].at.mapY, hud->racers[i].at.heading);
            }
        }
    }
    if (sRaceFrames++ % 60 != 0) {
        return;
    }
    bs_set_race_hud(hud);
    snprintf(name, sizeof(name), "race_%04d", sRaceFrames / 60);
    save(dir, name);
}

/* DKR_SETUP_SHOTS=dir: each page of the first-run setup (port_setup.c) as it changes, as setup_NNN.png. */
void host_bottom_setup(const char *dir, const SetupScreen *screen) {
    static int sStarted, sShots;
    char name[32];
    if (!sStarted) {
        sStarted = 1;
        bs_init(NULL);
        bs_load_assets();
    }
    bs_set_setup(screen);
    if (screen != NULL) {
        snprintf(name, sizeof(name), "setup_%03d", sShots++);
        save(dir, name);
    }
}
