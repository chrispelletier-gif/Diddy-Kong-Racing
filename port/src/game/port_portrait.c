/*
 * Portrait camera, a test helper for drawing character portraits from the game's own 3D models (the bottom screen's
 * boss and Taj portraits). Off unless DKR_PORTRAIT is set:
 *
 *   DKR_PORTRAIT=boss | taj | player   What to look at: the boss racer, Taj in the hub, or the player's racer.
 *   DKR_PORTRAIT_DIST=n                How far in front of it the camera stands (default 80).
 *   DKR_PORTRAIT_HEIGHT=n              How high above its origin the camera looks (default 30).
 *   DKR_PORTRAIT_YAW=n                 Turns the camera around it, in degrees (default 0; the bosses face it at
 *                                      about 90, see port/tools/portraits).
 *   DKR_PORTRAIT_LOOK=n                Which way the camera looks, in degrees from its own direction (default 180).
 *
 * While it is on, the track, the sky and every other object are left out, so the frame (DKR_DUMP_EVERY) is the model
 * alone on the clear colour, which the portrait script keys out.
 */
#include <ultra64.h>
#include "types.h"
#include "structs.h"
#include "camera.h"
#include "objects.h"
#include "racer.h"
#include "math_util.h"
#include "port.h"
#include "plat/plat.h"

static s32 sMode = -1; // 0 off, 1 boss, 2 Taj, 3 player.
static s32 sDist = 80, sHeight = 30, sYaw = 0, sLook = 0x8000;

static s32 setting(const char *name, s32 fallback) {
    const char *value = plat_getenv(name);
    return value != NULL ? port_atoi(value) : fallback;
}

static s32 mode(void) {
    if (sMode < 0) {
        const char *value = plat_getenv("DKR_PORTRAIT");
        sMode = value == NULL ? 0 : value[0] == 'b' ? 1 : value[0] == 't' ? 2 : 3;
        sDist = setting("DKR_PORTRAIT_DIST", 80);
        sHeight = setting("DKR_PORTRAIT_HEIGHT", 30);
        sYaw = setting("DKR_PORTRAIT_YAW", 0) * 0x10000 / 360;
        sLook = setting("DKR_PORTRAIT_LOOK", 180) * 0x10000 / 360;
    }
    return sMode;
}

s32 port_portrait_active(void) {
    return mode() != 0;
}

static Object *target(void) {
    s32 i, count, first;
    Object **racers;
    switch (mode()) {
        case 1:
            racers = get_racer_objects(&count);
            for (i = 0; i < count; i++) {
                if (racers[i]->racer->vehicleID >= VEHICLE_BOSSES) {
                    return racers[i];
                }
            }
            return NULL;
        case 2:
            first = get_first_active_object(&count);
            for (i = first; i < count; i++) {
                Object *obj = get_object(i);
                if (obj != NULL && obj->behaviorId == BHV_PARK_WARDEN) {
                    return obj;
                }
            }
            return NULL;
        case 3:
            return get_racer_object(PLAYER_ONE);
    }
    return NULL;
}

/* TRUE for every object but the one the portrait is of. */
s32 port_portrait_hides(Object *obj) {
    return mode() != 0 && obj != target();
}

/* Puts the camera in front of the subject, looking at it. */
void port_portrait_camera(Camera *cam) {
    Object *obj;
    s32 yaw;
    if (mode() == 0 || (obj = target()) == NULL) {
        return;
    }
    yaw = obj->trans.rotation.y_rotation + sYaw;
    cam->trans.x_position = obj->trans.x_position + sins_f(-yaw) * sDist;
    cam->trans.z_position = obj->trans.z_position + coss_f(-yaw) * sDist;
    cam->trans.y_position = obj->trans.y_position + sHeight;
    cam->trans.rotation.y_rotation = yaw + sLook;
    cam->trans.rotation.x_rotation = 0;
    cam->trans.rotation.z_rotation = 0;
    cam->pitch = 0;
}
