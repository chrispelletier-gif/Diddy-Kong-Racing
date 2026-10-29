/*
 * 3DS controls, mapped onto an N64 controller. By default:
 *   Circle Pad -> stick        A / B -> A / B        L, ZL -> Z (use item)     R, ZR -> R (hop/drift)
 *   X -> C-Up   Y -> C-Left    C-Stick -> C buttons  D-Pad -> D-Pad            SELECT -> L    START -> START
 * A B X Y L R ZL ZR SELECT can be remapped, and the circle pad's deadzone and response set, from the settings menu
 * (plat/controls.c). START, the D-Pad and the C-Stick stay fixed so the menus can always be reached.
 */
#include <3ds.h>
#include <math.h>
#include <string.h>

#include "plat/controls.h"
#include "plat/plat.h"

int update_3ds_touch(int x, int y);

typedef struct PortControllerState {
    u16 buttons;
    s8 stickX;
    s8 stickY;
    u8 connected;
} PortControllerState;

/* Remappable buttons (controls.c), in CTRL_KEY_* order. */
static const u32 sRemapKeys[CTRL_KEY_COUNT] = { KEY_A, KEY_B, KEY_X, KEY_Y, KEY_L, KEY_R, KEY_ZL, KEY_ZR, KEY_SELECT };

/* Buttons that always do the same thing. */
static const struct {
    u32 key;
    u16 n64;
} sFixedMap[] = {
    { KEY_START, CTRL_N64_START },    { KEY_DUP, CTRL_N64_DU },         { KEY_DDOWN, CTRL_N64_DD },
    { KEY_DLEFT, CTRL_N64_DL },       { KEY_DRIGHT, CTRL_N64_DR },      { KEY_CSTICK_UP, CTRL_N64_CU },
    { KEY_CSTICK_DOWN, CTRL_N64_CD }, { KEY_CSTICK_LEFT, CTRL_N64_CL }, { KEY_CSTICK_RIGHT, CTRL_N64_CR },
};

/* How far the circle pad reaches from its centre. */
#define CIRCLE_PAD_RANGE 155

/*
 * Tilt steering (roadmap O4b, controls.c): the accelerometer's pull of gravity gives how far the console is turned
 * like a steering wheel (roll about the axis through the screens), whichever way it is pitched; there is no drift to
 * correct as there would be adding up the gyroscope's turn rates. It is smoothed against the sensor's jitter, centred
 * on how the console was held when the race started, and only steers while racing, so the menus never move by
 * themselves. The accelerometer is only on while the setting is.
 */
static float tilt_roll(void) {
    static int sOn, sZeroPolls, sWasRacing, sCentreNow;
    static float sSmooth, sCentre;
    PlatGameNote note = { 0 };
    accelVector a;
    float roll;
    int racing;

    if (!controls_tilt_enabled()) {
        if (sOn) {
            HIDUSER_DisableAccelerometer();
            sOn = 0;
        }
        sCentreNow = 1; // Turned on again (mid-race too), it centres on how the console is held then.
        return 0.0f;
    }
    if (!sOn || sZeroPolls > 30) {
        // On; again if it stopped answering (after the console slept).
        HIDUSER_EnableAccelerometer();
        sOn = 1;
        sZeroPolls = 0;
    }
    hidAccelRead(&a);
    if (a.x == 0 && a.y == 0 && a.z == 0) {
        sZeroPolls++;
        return 0.0f;
    }
    sZeroPolls = 0;
    // The accelerometer's x is positive with the console turned to the left (checked on a New 3DS).
    roll = -atan2f((float) a.x, sqrtf((float) a.y * a.y + (float) a.z * a.z)) * (180.0f / 3.14159265f);
    racing = plat_get_game_note(&note) ? note.racing != 0 : sWasRacing;
    if (sCentreNow || (racing && !sWasRacing)) {
        // Centred on this reading itself, not on the smoothing still catching up with it.
        sSmooth = roll;
        sCentre = roll;
        sCentreNow = 0;
    }
    sSmooth += (roll - sSmooth) * 0.35f;
    sWasRacing = racing;
    // Not while paused: the pause menu and its options take the stick too.
    return racing && !note.paused ? sSmooth - sCentre : 0.0f;
}

void port_input_poll(PortControllerState *state, s32 maxControllers) {
    circlePosition pad;
    u32 held, remapped;
    unsigned i;
    s32 c;

    for (c = 0; c < maxControllers; c++) {
        memset(&state[c], 0, sizeof(state[c]));
    }
    if (maxControllers <= 0) {
        return;
    }

    hidScanInput();
    held = hidKeysHeld();
    // Only the buttons at the bottom of the screen respond to touch (update_3ds.c); a stray tap elsewhere does
    // nothing. SELECT + touch anywhere sends a bug report (bugreport_3ds.c), except in a player's build.
    if ((hidKeysDown() & KEY_TOUCH) && !(PLAT_DEBUG_TOOLS && (held & KEY_SELECT))) {
        touchPosition touch;
        hidTouchRead(&touch);
        update_3ds_touch(touch.px, touch.py);
    }
    hidCircleRead(&pad);

    state[0].connected = 1;
    remapped = 0;
    for (i = 0; i < CTRL_KEY_COUNT; i++) {
        if (held & sRemapKeys[i]) {
            remapped |= 1u << i;
        }
    }
    state[0].buttons = controls_map_buttons(remapped);
    for (i = 0; i < sizeof(sFixedMap) / sizeof(sFixedMap[0]); i++) {
        if (held & sFixedMap[i].key) {
            state[0].buttons |= sFixedMap[i].n64;
        }
    }
    controls_map_stick(pad.dx, pad.dy, CIRCLE_PAD_RANGE, &state[0].stickX, &state[0].stickY);
    controls_apply_tilt(tilt_roll(), &state[0].stickX); // Nothing with tilt steering off.

    // A test script (DKR_INPUT in env.txt) replaces the real controls.
    {
        static int sPolls;
        unsigned short buttons = 0;
        signed char x = 0, y = 0;
        if (plat_input_script(sPolls++, &buttons, &x, &y)) {
            state[0].buttons = buttons;
            state[0].stickX = x;
            state[0].stickY = y;
        }
    }
}

void port_rumble(s32 controller, s32 enable) {
    (void) controller;
    (void) enable;
}
