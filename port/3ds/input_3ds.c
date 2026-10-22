/*
 * 3DS controls, mapped onto an N64 controller:
 *   Circle Pad -> stick        A / B -> A / B        L, ZL -> Z (use item)     R, ZR -> R (hop/drift)
 *   X -> C-Up   Y -> C-Left    C-Stick -> C buttons  D-Pad -> D-Pad            SELECT -> L    START -> START
 */
#include <3ds.h>
#include <string.h>

#include "plat/plat.h"

int update_3ds_touch(int x, int y);

typedef struct PortControllerState {
    u16 buttons;
    s8 stickX;
    s8 stickY;
    u8 connected;
} PortControllerState;

enum {
    N64_A = 0x8000,
    N64_B = 0x4000,
    N64_Z = 0x2000,
    N64_START = 0x1000,
    N64_DU = 0x0800,
    N64_DD = 0x0400,
    N64_DL = 0x0200,
    N64_DR = 0x0100,
    N64_L = 0x0020,
    N64_R = 0x0010,
    N64_CU = 0x0008,
    N64_CD = 0x0004,
    N64_CL = 0x0002,
    N64_CR = 0x0001,
};

static const struct {
    u32 key;
    u16 n64;
} sButtonMap[] = {
    { KEY_A, N64_A },          { KEY_B, N64_B },           { KEY_L, N64_Z },          { KEY_ZL, N64_Z },
    { KEY_R, N64_R },          { KEY_ZR, N64_R },          { KEY_SELECT, N64_L },     { KEY_START, N64_START },
    { KEY_X, N64_CU },         { KEY_Y, N64_CL },          { KEY_DUP, N64_DU },       { KEY_DDOWN, N64_DD },
    { KEY_DLEFT, N64_DL },     { KEY_DRIGHT, N64_DR },     { KEY_CSTICK_UP, N64_CU }, { KEY_CSTICK_DOWN, N64_CD },
    { KEY_CSTICK_LEFT, N64_CL }, { KEY_CSTICK_RIGHT, N64_CR },
};

/* The circle pad reaches about +-155; an N64 stick about +-80. */
static s8 scale_axis(int v) {
    v = v * 85 / 150;
    if (v > 80) {
        v = 80;
    } else if (v < -80) {
        v = -80;
    }
    return (s8) v;
}

void port_input_poll(PortControllerState *state, s32 maxControllers) {
    circlePosition pad;
    u32 held;
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
    // nothing. SELECT + touch anywhere sends a bug report (bugreport_3ds.c).
    if ((hidKeysDown() & KEY_TOUCH) && !(held & KEY_SELECT)) {
        touchPosition touch;
        hidTouchRead(&touch);
        update_3ds_touch(touch.px, touch.py);
    }
    hidCircleRead(&pad);

    state[0].connected = 1;
    for (i = 0; i < sizeof(sButtonMap) / sizeof(sButtonMap[0]); i++) {
        if (held & sButtonMap[i].key) {
            state[0].buttons |= sButtonMap[i].n64;
        }
    }
    state[0].stickX = scale_axis(pad.dx);
    state[0].stickY = scale_axis(pad.dy);

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
