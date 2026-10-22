/* Nintendo 3DS frontend: system setup and the main (APT) loop. */
#include <3ds.h>
#include <stdio.h>
#include <sys/stat.h>

#include "plat/plat.h"

void port_boot(void);
void port_si_flush(void);
int bugreport_boot(void);
void bugreport_start(void);
void bugreport_clean_exit(void);
void bugreport_watch_thread(const char *name);
void plat_3ds_console_init(void);
void update_3ds_start(const char *selfPath);

static volatile int sQuitRequested;

/* Ends the game the normal way, from another thread (the update's "Quit to finish the update"). */
void plat_3ds_request_quit(void) {
    sQuitRequested = 1;
}

/* Saves are written to the SD card shortly after the game makes them; make sure none are lost when leaving. */
static aptHookCookie sAptCookie;
static void apt_hook(APT_HookType hook, void *param) {
    (void) param;
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP || hook == APTHOOK_ONEXIT) {
        port_si_flush();
    }
    // The renderer must not draw while the HOME menu has the GPU.
    if (hook == APTHOOK_ONSUSPEND) {
        plat_3ds_pause_renderer(1);
    } else if (hook == APTHOOK_ONRESTORE) {
        plat_3ds_pause_renderer(0);
    }
}

/* The game only uses the top screen; the bottom one shows the controls. */
static void show_bottom_screen_help(void) {
    printf("\n  Diddy Kong Racing\n");
    printf("  Native 3DS port of the decomp\n\n");
    printf("  Circle Pad   Steer\n");
    printf("  A            Accelerate\n");
    printf("  B            Brake / reverse\n");
    printf("  L / ZL       Use item (Z)\n");
    printf("  R / ZR       Hop / drift\n");
    printf("  X / Y        C-Up / C-Left\n");
    printf("  C-Stick      C buttons\n");
    printf("  SELECT       N64 L button\n");
    printf("  START        Pause\n\n");
    printf("  SELECT+touch Send a bug report\n\n");
    printf("  Buttons below: send a bug report,\n");
    printf("  start an unattended test run,\n");
    printf("  install a newer build\n\n");
    printf("  Saves: sdmc:/3ds/dkr/\n");
}

int plat_main_loop_iteration(void) {
    if (!aptMainLoop() || sQuitRequested) {
        return 0;
    }
    // Back in the foreground (not every way back calls the restore hook).
    plat_3ds_pause_renderer(0);
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

int main(int argc, char **argv) {

    gfxInitDefault();
    gfxSet3D(false);
    hidInit();
    mkdir("sdmc:/3ds", 0777);
    mkdir(plat_data_dir(), 0777);
    plat_3ds_console_init();
    bugreport_watch_thread("main");
    // Offers to report the last run if it did not end normally. Must come before anything is logged.
    if (bugreport_boot()) {
        consoleClear();
    }
    show_bottom_screen_help();

    port_boot();
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
