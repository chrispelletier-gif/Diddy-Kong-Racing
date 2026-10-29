/*
 * Player controls: which console button presses which N64 button, and how the stick is read (deadzone, range and
 * response curve). Platform neutral; the platform's input code reads its buttons into CTRL_KEY_* bits and its stick
 * in its own units, and these turn them into N64 controller values.
 *
 * The settings are kept in controls.txt in the data directory. The settings menu shows them through the option list
 * below, so it needs no knowledge of what each option is. The input poll (game thread) maps buttons and the stick
 * while the menu (another thread) may change a setting; every setting is stored as whole ints, so a poll sees either
 * the old value or the new one.
 */
#ifndef PLAT_CONTROLS_H
#define PLAT_CONTROLS_H

/* The console buttons that can be remapped. */
enum {
    CTRL_KEY_A,
    CTRL_KEY_B,
    CTRL_KEY_X,
    CTRL_KEY_Y,
    CTRL_KEY_L,
    CTRL_KEY_R,
    CTRL_KEY_ZL,
    CTRL_KEY_ZR,
    CTRL_KEY_SELECT,
    CTRL_KEY_COUNT
};

/* N64 controller buttons. */
enum {
    CTRL_N64_A = 0x8000,
    CTRL_N64_B = 0x4000,
    CTRL_N64_Z = 0x2000,
    CTRL_N64_START = 0x1000,
    CTRL_N64_DU = 0x0800,
    CTRL_N64_DD = 0x0400,
    CTRL_N64_DL = 0x0200,
    CTRL_N64_DR = 0x0100,
    CTRL_N64_L = 0x0020,
    CTRL_N64_R = 0x0010,
    CTRL_N64_CU = 0x0008,
    CTRL_N64_CD = 0x0004,
    CTRL_N64_CL = 0x0002,
    CTRL_N64_CR = 0x0001,
    /* Not an N64 button: the port's look-back camera (PORT_BUTTON_LOOK_BACK in port.h; os_si.c takes it out). */
    CTRL_N64_LOOK_BACK = 0x0040,
};

/* Loads controls.txt (once; later calls do nothing). */
void controls_init(void);

/* The N64 buttons pressed by the held console buttons (bit n = CTRL_KEY_n). */
unsigned short controls_map_buttons(unsigned int heldKeys);

/*
 * The N64 stick (-80..80 per axis) for a raw stick position, where fullRange is how far the raw stick reaches from
 * its centre. The deadzone and curve are applied to the distance from the centre, so the direction is kept.
 */
void controls_map_stick(int rawX, int rawY, int fullRange, signed char *outX, signed char *outY);

/*
 * Tilt steering (roadmap O4b): whether it is on, and adding a tilt of the console (degrees of roll from where it was
 * held when the race started, positive to the right) to the N64 stick's x. The platform reads the tilt; the setting
 * decides the direction and how far a tilt goes.
 */
int controls_tilt_enabled(void);
void controls_apply_tilt(float roll, signed char *outX);

/* The last stick position mapped, raw and converted, for a live readout in the settings menu. */
void controls_last_stick(int *rawX, int *rawY, int *outX, int *outY);

/*
 * Option list for the settings menu. Each option has a label and a value text; change steps it by -1 or +1 (wrapping
 * round) and saves controls.txt. The "Reset" option puts every setting back to its default on any step.
 */
/*
 * For the bottom screen's controls card: the console buttons that do an action, as "R / ZR" (actions: CTRL_N64_*;
 * any C button for CTRL_N64_CU | CTRL_N64_CL...), or "" when none does.
 */
void controls_keys_for(unsigned short actions, char *out, int size);

int controls_option_count(void);
const char *controls_option_label(int index);
void controls_option_value(int index, char *out, int size);
void controls_option_change(int index, int step);

#endif
