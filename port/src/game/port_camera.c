/*
 * Camera options (port.h): the Modern chase camera setting and the look-back button's state. The camera code that
 * uses them is in src/racer.c (update_camera_car, update_camera_hovercraft, update_camera_plane and
 * update_player_camera), behind TARGET_PORT.
 *
 * The setting is kept as camera= in settings.txt: 0 the N64's own camera (the default), 1 Modern. DKR_CAMERA in
 * env.txt (or the environment on PC) sets it for a test run. The wall check (camera_wall_check in src/racer.c) is
 * camwall= (1, on, by default; 0 the N64's camera that passes into walls), DKR_CAMERA_WALLS for a test. The Settings menu changes it from its own thread while
 * the game reads it; it is a single int, so the game sees either the old value or the new one.
 */
#include <stdlib.h>

#include "plat/plat.h"
#include "plat/settings.h"
#include "port.h"

static volatile s32 sModernCamera = -1; // -1 until loaded.
static volatile s32 sLookBackHeld;
static volatile s32 sWallCheck = -1; // -1 until loaded.

s32 port_modern_camera(void) {
    if (sModernCamera < 0) {
        const char *env = plat_getenv("DKR_CAMERA");
        sModernCamera = env != NULL ? atoi(env) != 0 : settings_get("camera", 0) != 0;
    }
    return sModernCamera;
}

void port_set_modern_camera(s32 on) {
    sModernCamera = on != 0;
    settings_set("camera", sModernCamera);
}

s32 port_camera_wall_check(void) {
    if (sWallCheck < 0) {
        const char *env = plat_getenv("DKR_CAMERA_WALLS");
        sWallCheck = env != NULL ? atoi(env) != 0 : settings_get("camwall", 1) != 0;
    }
    return sWallCheck;
}

void port_set_camera_wall_check(s32 on) {
    sWallCheck = on != 0;
    settings_set("camwall", sWallCheck);
}

s32 port_look_back_held(void) {
    return sLookBackHeld;
}

void port_set_look_back_held(s32 held) {
    sLookBackHeld = held;
}

/* The Camera page of the Settings menu (menu_3ds_add_page on the 3DS). */
int camera_option_count(void) {
    return 2;
}

const char *camera_option_label(int index) {
    return index == 0 ? "Camera style" : "Wall check";
}

void camera_option_value(int index, char *out, int size) {
    if (index == 0) {
        plat_snprintf(out, size, "%s", port_modern_camera() ? "Modern" : "Original");
    } else {
        plat_snprintf(out, size, "%s", port_camera_wall_check() ? "On" : "Off");
    }
}

void camera_option_change(int index, int step) {
    (void) step;
    if (index == 0) {
        port_set_modern_camera(!port_modern_camera());
    } else {
        port_set_camera_wall_check(!port_camera_wall_check());
    }
}
