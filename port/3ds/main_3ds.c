/* Nintendo 3DS frontend: system setup and the main (APT) loop. */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "menu_3ds.h"
#include "plat/controls.h"
#include "plat/options.h"
#include "plat/plat.h"

void port_boot(void);
void port_si_flush(void);
int bugreport_boot(void);
void bugreport_start(void);
void bugreport_clean_exit(void);
void bugreport_watch_thread(const char *name);
void plat_3ds_console_init(void);
void plat_3ds_console_line(const char *text);
void plat_3ds_console_clear(void);
void update_3ds_start(const char *selfPath);
/* The Sound page (audio_ndsp.c). */
int sound_option_count(void);
const char *sound_option_label(int i);
void sound_option_value(int i, char *out, int size);
void sound_option_change(int i, int step);
int sound_option_in_race(int i);
int port_autorun_enabled(void);
/* The Camera page (port_camera.c). */
int race_hud_option_count(void);
const char *race_hud_option_label(int i);
void race_hud_option_value(int i, char *out, int size);
void race_hud_option_change(int i, int step);
const char *race_hud_option_hint(int i);
int camera_option_count(void);
const char *camera_option_label(int i);
void camera_option_value(int i, char *out, int size);
void camera_option_change(int i, int step);
int port_modern_camera(void);
/* The Start-up page (port_intro.c). */
int intro_option_count(void);
const char *intro_option_label(int i);
void intro_option_value(int i, char *out, int size);
void intro_option_change(int i, int step);
const char *intro_option_hint(int i);
int intro_option_in_race(int i);
/* The Saves page (saves_3ds.c). */
int saves_option_count(void);
const char *saves_option_label(int i);
void saves_option_value(int i, char *out, int size);
void saves_option_change(int i, int step);
const char *saves_option_hint(int i);

static volatile int sQuitRequested;

void plat_request_quit(void) {
    sQuitRequested = 1;
}

/*
 * Leaving the game for the HOME menu or closing the lid. Saves are written to the SD card shortly after the game makes
 * them; make sure none are lost. The renderer must not draw while the HOME menu has the GPU or the console sleeps (the
 * GPU hung when it did), so it waits at the start of a frame, which also holds the game and its sound, until the game
 * is back. And a race is paused, as START would, so it does not carry on the moment the game comes back.
 */
static aptHookCookie sAptCookie;
static void apt_hook(APT_HookType hook, void *param) {
    (void) param;
    switch (hook) {
        case APTHOOK_ONSUSPEND:
        case APTHOOK_ONSLEEP:
            plat_log("%s: pausing", hook == APTHOOK_ONSLEEP ? "sleep" : "home menu");
            port_si_flush();
            plat_request_game_pause();
            plat_3ds_pause_renderer(1);
            break;
        case APTHOOK_ONRESTORE:
        case APTHOOK_ONWAKEUP:
            plat_3ds_pause_renderer(0);
            break;
        case APTHOOK_ONEXIT:
            port_si_flush();
            break;
        default:
            break;
    }
}

/* Sleep holds; while any is held, closing the lid does not put the console to sleep. */
static LightLock sSleepLock = 1; // Unlocked.
static int sSleepHolds;

/*
 * Network transfers (bug reports, updates) would lose their connection in sleep mode, so they keep the console awake
 * while they run.
 */
void plat_3ds_hold_sleep(int hold) {
    LightLock_Lock(&sSleepLock);
    sSleepHolds += hold ? 1 : -1;
    aptSetSleepAllowed(sSleepHolds <= 0);
    LightLock_Unlock(&sSleepLock);
}

int plat_main_loop_iteration(void) {
    if (!aptMainLoop() || sQuitRequested) {
        return 0;
    }
    // Back in the foreground (not every way back calls the restore hook).
    plat_3ds_pause_renderer(0);
    {
        // An unattended test run keeps going with the lid closed (the screens turn off, the race does not stop).
        static int sTestRunHold;
        int testRun = port_autorun_enabled() != 0;
        if (testRun != sTestRunHold) {
            sTestRunHold = testRun;
            plat_3ds_hold_sleep(testRun);
        }
    }
    // The game runs on its own threads; the main thread only has to keep servicing APT (home menu, sleep).
    svcSleepThread(16 * 1000 * 1000);
    gPlatCounters[PLAT_COUNTER_MAIN]++;
    {
        // For the start-up log: shows whether the main thread still gets to run (it is the lowest priority one).
        static int sIterations;
        if (++sIterations % 120 == 0 && sIterations <= 120 * 15) {
            plat_log("main thread alive");
        }
    }
    return 1;
}

/* The same settings in the game's own Options and pause menus (plat/options.h, src/menu.c). */
static void add_game_menu_options(void) {
    static const OptionsSection sCamera = { camera_option_count, camera_option_label, camera_option_value,
                                            camera_option_change, NULL, NULL };
    static const OptionsSection sSound = { sound_option_count, sound_option_label, sound_option_value,
                                           sound_option_change, NULL, sound_option_in_race };
    static const OptionsSection sControls = { controls_option_count, controls_option_label, controls_option_value,
                                              controls_option_change, NULL, NULL };
    static const OptionsSection sRaceHud = { race_hud_option_count, race_hud_option_label, race_hud_option_value,
                                             race_hud_option_change, race_hud_option_hint, NULL };
    static const OptionsSection sIntro = { intro_option_count, intro_option_label, intro_option_value,
                                           intro_option_change, intro_option_hint, intro_option_in_race };
    menu_3ds_add_options();
    options_add(OPTIONS_SCREEN, &sRaceHud);
    options_add(OPTIONS_GENERAL, &sIntro);
    options_add(OPTIONS_CAMERA, &sCamera);
    options_add(OPTIONS_AUDIO, &sSound);
    options_add(OPTIONS_CONTROLS, &sControls);
}

int main(int argc, char **argv) {

    {
        /* DKR_VRAM_SCREEN=1 puts the screens' buffers in VRAM instead of FCRAM, a test for the white line. Off by
         * default: on a New 3DS the CPU may not write there (a data abort drawing the bottom screen), so with it the
         * bottom screen stays blank. */
        const char *env = plat_getenv("DKR_VRAM_SCREEN");
        gfxInit(GSP_BGR8_OES, GSP_BGR8_OES, env != NULL && atoi(env) != 0);
    }
    gfxSet3D(false);
    hidInit();
    mkdir("sdmc:/3ds", 0777);
    mkdir(plat_data_dir(), 0777);
    plat_3ds_console_init();
    bugreport_watch_thread("main");
    // Offers to report the last run if it did not end normally. Must come before anything is logged.
    if (bugreport_boot()) {
        plat_3ds_console_clear();
    }
    // The bottom screen shows the controls itself (plat/bottom_screen.c).
    plat_3ds_console_line("Saves and settings: sdmc:/3ds/dkr/");

    port_boot();
    // Loaded here, before the menu's thread can read or change them.
    controls_init();
    menu_3ds_add_page("Controls", controls_option_count, controls_option_label, controls_option_value,
                      controls_option_change);
    menu_3ds_add_page("Sound", sound_option_count, sound_option_label, sound_option_value, sound_option_change);
    port_modern_camera();
    menu_3ds_add_page("Race HUD", race_hud_option_count, race_hud_option_label, race_hud_option_value,
                      race_hud_option_change);
    menu_3ds_add_page("Camera", camera_option_count, camera_option_label, camera_option_value, camera_option_change);
    menu_3ds_add_page("Start-up", intro_option_count, intro_option_label, intro_option_value, intro_option_change);
    menu_3ds_add_page_hint("Saves", saves_option_count, saves_option_label, saves_option_value, saves_option_change,
                           saves_option_hint, "Saved as N64 dkr.eep and dkr.mpk");
    add_game_menu_options();
    bugreport_start();
    // The Homebrew Launcher passes the path of the .3dsx as argv[0].
    update_3ds_start(argc > 0 ? argv[0] : NULL);
    aptHook(&sAptCookie, apt_hook, NULL);
    while (plat_main_loop_iteration()) {
    }
    port_si_flush();
    bugreport_clean_exit();
    plat_3ds_exit();
}
