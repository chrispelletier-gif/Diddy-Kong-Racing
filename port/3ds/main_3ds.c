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

/* Saves are written to the SD card shortly after the game makes them; make sure none are lost when leaving. */
static aptHookCookie sAptCookie;
static void apt_hook(APT_HookType hook, void *param) {
    (void) param;
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP || hook == APTHOOK_ONEXIT) {
        port_si_flush();
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
    printf("  Saves: sdmc:/3ds/dkr/\n");
}

int plat_main_loop_iteration(void) {
    if (!aptMainLoop()) {
        return 0;
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

int main(int argc, char **argv) {
    (void) argc;
    (void) argv;

    gfxInitDefault();
    gfxSet3D(false);
    hidInit();
    mkdir("sdmc:/3ds", 0777);
    mkdir(plat_data_dir(), 0777);
    {
        static PrintConsole sConsole;
        consoleInit(GFX_BOTTOM, &sConsole);
    }
    bugreport_watch_thread("main");
    // Offers to report the last run if it did not end normally. Must come before anything is logged.
    if (bugreport_boot()) {
        consoleClear();
    }
    show_bottom_screen_help();

    port_boot();
    bugreport_start();
    aptHook(&sAptCookie, apt_hook, NULL);
    while (plat_main_loop_iteration()) {
    }
    port_si_flush();
    gfxExit();
    bugreport_clean_exit();
    return 0;
}
