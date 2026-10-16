/* Nintendo 3DS frontend: system setup and the main (APT) loop. */
#include <3ds.h>
#include <sys/stat.h>

#include "plat/plat.h"

void port_boot(void);

int plat_main_loop_iteration(void) {
    if (!aptMainLoop()) {
        return 0;
    }
    // The game runs on its own threads; the main thread only has to keep servicing APT (home menu, sleep).
    svcSleepThread(16 * 1000 * 1000);
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

    port_boot();
    while (plat_main_loop_iteration()) {
    }
    gfxExit();
    return 0;
}
