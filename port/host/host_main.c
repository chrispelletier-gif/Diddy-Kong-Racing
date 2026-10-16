/*
 * Host (PC) frontend used to test the port without 3DS hardware.
 * It runs the game headless with a software renderer that can dump frames to image files.
 *
 * Environment variables:
 *   DKR_DATA_DIR    directory containing baserom.z64 and save files (default: current directory)
 *   DKR_FRAMES      exit after rendering this many frames (default: run forever)
 *   DKR_DUMP_EVERY  write every Nth frame to DKR_DUMP_DIR/frame_XXXXX.ppm (default: 0 = never)
 *   DKR_INPUT       scripted input, see host_input.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "plat/plat.h"

void port_boot(void);
void port_si_flush(void);
int host_frames_rendered(void);

int plat_main_loop_iteration(void) {
    static int maxFrames = -1;
    if (maxFrames == -1) {
        const char *env = getenv("DKR_FRAMES");
        maxFrames = env != NULL ? atoi(env) : 0;
    }
    usleep(10000);
    if (maxFrames > 0 && host_frames_rendered() >= maxFrames) {
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    port_boot();
    while (plat_main_loop_iteration()) {
    }
    port_si_flush();
    fprintf(stderr, "Exiting after %d frames\n", host_frames_rendered());
    return 0;
}
