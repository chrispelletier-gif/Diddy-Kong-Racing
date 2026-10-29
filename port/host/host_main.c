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

#include "plat/controls.h"
#include "plat/options.h"
#include "plat/plat.h"

void port_boot(void);
void port_si_flush(void);
int host_frames_rendered(void);
int plat_60fps_setting(void);
void plat_set_60fps_setting(int on);
int camera_option_count(void);
const char *camera_option_label(int i);
void camera_option_value(int i, char *out, int size);
void camera_option_change(int i, int step);
int intro_option_count(void);
const char *intro_option_label(int i);
void intro_option_value(int i, char *out, int size);
void intro_option_change(int i, int step);
const char *intro_option_hint(int i);
int intro_option_in_race(int i);

/* The game menus' VIDEO OPTIONS on PC: the frame rate (the 3DS has its GPU's settings too, port/3ds/menu_3ds.c). */
static int video_count(void) {
    return 1;
}

static const char *video_label(int i) {
    (void) i;
    return "Frame rate";
}

static void video_value(int i, char *out, int size) {
    (void) i;
    snprintf(out, size, "%s", plat_60fps_setting() ? "60 fps" : "30 fps");
}

static void video_change(int i, int step) {
    (void) i;
    (void) step;
    plat_set_60fps_setting(!plat_60fps_setting());
}

/* The settings the game's Options and pause menus show (plat/options.h), so they can be tried on PC. */
static void add_game_menu_options(void) {
    static const OptionsSection sVideo = { video_count, video_label, video_value, video_change, NULL, NULL };
    static const OptionsSection sCamera = { camera_option_count, camera_option_label, camera_option_value,
                                            camera_option_change, NULL, NULL };
    static const OptionsSection sControls = { controls_option_count, controls_option_label, controls_option_value,
                                              controls_option_change, NULL, NULL };
    static const OptionsSection sIntro = { intro_option_count, intro_option_label, intro_option_value,
                                           intro_option_change, intro_option_hint, intro_option_in_race };
    options_add(OPTIONS_VIDEO, &sVideo);
    options_add(OPTIONS_GENERAL, &sIntro);
    options_add(OPTIONS_CAMERA, &sCamera);
    options_add(OPTIONS_CONTROLS, &sControls);
}

static volatile int sQuitRequested;

void plat_request_quit(void) {
    sQuitRequested = 1;
}

int plat_main_loop_iteration(void) {
    static int maxFrames = -1;
    if (maxFrames == -1) {
        const char *env = getenv("DKR_FRAMES");
        maxFrames = env != NULL ? atoi(env) : 0;
    }
    usleep(10000);
    if (sQuitRequested || (maxFrames > 0 && host_frames_rendered() >= maxFrames)) {
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    add_game_menu_options();
    port_boot();
    while (plat_main_loop_iteration()) {
    }
    port_si_flush();
    fprintf(stderr, "Exiting after %d frames\n", host_frames_rendered());
    return 0;
}
