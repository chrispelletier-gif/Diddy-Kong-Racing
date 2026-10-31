/*
 * Player controls: button mapping and stick response (see controls.h).
 *
 * controls.txt holds one "name=value" line per setting, for example "deadzone=6" or "map.ZL=LOOK"; unknown lines and
 * values are ignored, so an old or hand-edited file still loads.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat/controls.h"
#include "plat/plat.h"

#define CONTROLS_FILE "controls.txt"
/* 2: ZL looks back in the standard layouts (it was Z, as L). A file from before has no version line. */
#define CONTROLS_VERSION 2

/* An N64 stick reaches about +-80; with the full range at 100% the raw stick's edge gives this much, as before. */
#define STICK_MAX 80
#define STICK_EDGE 88.0f

#define DEADZONE_MAX 30
#define DEADZONE_STEP 2
#define DEADZONE_DEFAULT 6
#define SENSITIVITY_MIN 100
#define SENSITIVITY_MAX 150
#define SENSITIVITY_STEP 10
/* Tilt steering: Off, On, or On with the direction reversed; full lock at this many degrees of tilt. */
enum { TILT_OFF, TILT_ON, TILT_INVERTED, TILT_COUNT };
#define TILT_RANGE_MIN 10
#define TILT_RANGE_MAX 45
#define TILT_RANGE_STEP 5
#define TILT_RANGE_DEFAULT 25
#define TILT_DEADZONE 2.0f // Degrees either side of the centre that steer straight on.

enum { CURVE_LINEAR, CURVE_SOFT, CURVE_SOFTER, CURVE_QUICK, CURVE_COUNT };

static const char *const sCurveNames[CURVE_COUNT] = { "Linear", "Soft", "Softer", "Quick" };
static const char *const sTiltNames[TILT_COUNT] = { "Off", "On", "Reversed" };

/* What a console button can press. */
typedef struct Action {
    unsigned short n64;
    const char *name;  // In controls.txt.
    const char *label; // In the menu (at most 12 characters).
} Action;

static const Action sActions[] = {
    { CTRL_N64_A, "A", "Accelerate" },
    { CTRL_N64_B, "B", "Brake" },
    { CTRL_N64_Z, "Z", "Use item" },
    { CTRL_N64_R, "R", "Hop, drift" },
    { CTRL_N64_L, "L", "No effect" },
    { CTRL_N64_CU, "CU", "Camera view" },
    { CTRL_N64_CD, "CD", "Track arrows" },
    { CTRL_N64_CL, "CL", "No effect" },
    { CTRL_N64_CR, "CR", "HUD toggle" },
    { CTRL_N64_START, "START", "Start" },
    { CTRL_N64_LOOK_BACK, "LOOK", "Look back" },
    { 0, "NONE", "Nothing" },
};
#define ACTION_COUNT ((int) (sizeof(sActions) / sizeof(sActions[0])))

static const char *const sKeyNames[CTRL_KEY_COUNT] = { "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "SELECT" };

/* Layouts, as an action per console button. The first is the default. */
typedef struct Layout {
    const char *label;
    unsigned short map[CTRL_KEY_COUNT];
} Layout;

static const Layout sLayouts[] = {
    { "Standard",
      { CTRL_N64_A, CTRL_N64_B, CTRL_N64_CU, CTRL_N64_CL, CTRL_N64_Z, CTRL_N64_R, CTRL_N64_LOOK_BACK, CTRL_N64_R,
        CTRL_N64_L } },
    { "A/B swapped",
      { CTRL_N64_B, CTRL_N64_A, CTRL_N64_CU, CTRL_N64_CL, CTRL_N64_Z, CTRL_N64_R, CTRL_N64_LOOK_BACK, CTRL_N64_R,
        CTRL_N64_L } },
    { "L/R swapped",
      { CTRL_N64_A, CTRL_N64_B, CTRL_N64_CU, CTRL_N64_CL, CTRL_N64_R, CTRL_N64_Z, CTRL_N64_LOOK_BACK, CTRL_N64_Z,
        CTRL_N64_L } },
};
#define LAYOUT_COUNT ((int) (sizeof(sLayouts) / sizeof(sLayouts[0])))

static struct {
    unsigned short map[CTRL_KEY_COUNT];
    int deadzone;    // Percent of the stick's range.
    int sensitivity; // Percent: at 150 the stick gives full lock at two thirds of its travel.
    int curve;
    int tilt;      // TILT_*
    int tiltRange; // Degrees of tilt for full lock.
} sSettings;

static int sLoaded;
static int sFileVersion; // Of the controls.txt loaded; 0 for one from before versions.
static int sLastRaw[2];
static int sLastOut[2];

/* Option list rows. */
enum {
    OPT_LAYOUT,
    OPT_KEY_FIRST,
    OPT_DEADZONE = OPT_KEY_FIRST + CTRL_KEY_COUNT,
    OPT_SENSITIVITY,
    OPT_CURVE,
    OPT_TILT,
    OPT_TILT_RANGE,
    OPT_RESET,
    OPT_COUNT
};

/* Element by element, so the input poll never sees a slot half written. */
static void set_map(const unsigned short *map) {
    int i;
    for (i = 0; i < CTRL_KEY_COUNT; i++) {
        sSettings.map[i] = map[i];
    }
}

static void set_defaults(void) {
    set_map(sLayouts[0].map);
    sSettings.deadzone = DEADZONE_DEFAULT;
    sSettings.sensitivity = SENSITIVITY_MIN;
    sSettings.curve = CURVE_LINEAR;
    sSettings.tilt = TILT_OFF;
    sSettings.tiltRange = TILT_RANGE_DEFAULT;
}

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static int action_index(unsigned short n64) {
    int i;
    for (i = 0; i < ACTION_COUNT; i++) {
        if (sActions[i].n64 == n64) {
            return i;
        }
    }
    return ACTION_COUNT - 1;
}

/*
 * The game never reads the N64's L or C-Left, so they are not offered when remapping (an old controls.txt that has
 * them still loads, and shows them as "No effect").
 */
static int action_offered(int a) {
    return sActions[a].n64 != CTRL_N64_L && sActions[a].n64 != CTRL_N64_CL;
}

/* The layout the current mapping matches, or -1 for a custom one. */
static int current_layout(void) {
    int i;
    for (i = 0; i < LAYOUT_COUNT; i++) {
        if (memcmp(sLayouts[i].map, sSettings.map, sizeof(sSettings.map)) == 0) {
            return i;
        }
    }
    return -1;
}

static void file_path(char *path, int size) {
    plat_snprintf(path, size, "%s/" CONTROLS_FILE, plat_data_dir());
}

static void parse_line(const char *line) {
    char name[24];
    char value[24];
    int i;
    if (sscanf(line, " %23[^= ] = %23s", name, value) != 2) {
        return;
    }
    if (strcmp(name, "version") == 0) {
        sFileVersion = atoi(value);
    } else if (strcmp(name, "deadzone") == 0) {
        sSettings.deadzone = clamp(atoi(value), 0, DEADZONE_MAX);
    } else if (strcmp(name, "sensitivity") == 0) {
        sSettings.sensitivity = clamp(atoi(value), SENSITIVITY_MIN, SENSITIVITY_MAX);
    } else if (strcmp(name, "curve") == 0) {
        sSettings.curve = clamp(atoi(value), 0, CURVE_COUNT - 1);
    } else if (strcmp(name, "tilt") == 0) {
        sSettings.tilt = clamp(atoi(value), 0, TILT_COUNT - 1);
    } else if (strcmp(name, "tilt_range") == 0) {
        sSettings.tiltRange = clamp(atoi(value), TILT_RANGE_MIN, TILT_RANGE_MAX);
    } else if (strncmp(name, "map.", 4) == 0) {
        for (i = 0; i < CTRL_KEY_COUNT; i++) {
            if (strcmp(name + 4, sKeyNames[i]) == 0) {
                int a;
                for (a = 0; a < ACTION_COUNT; a++) {
                    if (strcmp(value, sActions[a].name) == 0) {
                        sSettings.map[i] = sActions[a].n64;
                    }
                }
            }
        }
    }
}

static void save(void) {
    char path[256];
    char text[512];
    int length, i;

    length =
        plat_snprintf(text, sizeof(text), "version=%d\ndeadzone=%d\nsensitivity=%d\ncurve=%d\ntilt=%d\ntilt_range=%d\n",
                      CONTROLS_VERSION, sSettings.deadzone, sSettings.sensitivity, sSettings.curve, sSettings.tilt,
                      sSettings.tiltRange);
    for (i = 0; i < CTRL_KEY_COUNT && length < (int) sizeof(text); i++) {
        length += plat_snprintf(text + length, sizeof(text) - length, "map.%s=%s\n", sKeyNames[i],
                                sActions[action_index(sSettings.map[i])].name);
    }
    file_path(path, sizeof(path));
    if (!plat_save_file(path, text, (unsigned int) strlen(text))) {
        plat_log("controls: could not save %s", path);
    }
}

void controls_init(void) {
    char path[256];
    char text[1024];
    char *line;
    int length;

    if (sLoaded) {
        return;
    }
    set_defaults();
    file_path(path, sizeof(path));
    length = plat_load_file_start(path, text, sizeof(text) - 1);
    if (length <= 0) {
        sLoaded = 1;
        return;
    }
    text[length] = '\0';
    for (line = text; line != NULL && *line != '\0';) {
        char *next = strchr(line, '\n');
        if (next != NULL) {
            *next++ = '\0';
        }
        parse_line(line);
        line = next;
    }
    if (sFileVersion < 2 && sSettings.map[CTRL_KEY_ZL] == CTRL_N64_Z) {
        // Saved before ZL looked back, when it only did what L does: it looks back now, as in a new layout.
        sSettings.map[CTRL_KEY_ZL] = CTRL_N64_LOOK_BACK;
    }
    sLoaded = 1;
    plat_log("controls: deadzone %d%%, sensitivity %d%%, %s, layout %s, tilt %s (%d degrees)", sSettings.deadzone,
             sSettings.sensitivity, sCurveNames[sSettings.curve],
             current_layout() >= 0 ? sLayouts[current_layout()].label : "custom", sTiltNames[sSettings.tilt],
             sSettings.tiltRange);
}

unsigned short controls_map_buttons(unsigned int heldKeys) {
    unsigned short buttons = 0;
    int i;
    controls_init();
    for (i = 0; i < CTRL_KEY_COUNT; i++) {
        if (heldKeys & (1u << i)) {
            buttons |= sSettings.map[i];
        }
    }
    return buttons;
}

/* The response curve, on the part of the stick's travel past the deadzone (0..1). */
static float apply_curve(float t) {
    switch (sSettings.curve) {
        case CURVE_SOFT:
            return t * (1.0f + t) * 0.5f;
        case CURVE_SOFTER:
            return t * t;
        case CURVE_QUICK:
            return t * (2.0f - t);
        default:
            return t;
    }
}

void controls_map_stick(int rawX, int rawY, int fullRange, signed char *outX, signed char *outY) {
    float distance, deadzone, full, t, scale;
    int x = 0, y = 0;

    controls_init();
    sLastRaw[0] = rawX;
    sLastRaw[1] = rawY;
    distance = sqrtf((float) (rawX * rawX + rawY * rawY));
    deadzone = fullRange * sSettings.deadzone / 100.0f;
    full = fullRange * 100.0f / sSettings.sensitivity;
    if (distance > deadzone && full > deadzone) {
        t = (distance - deadzone) / (full - deadzone);
        if (t > 1.0f) {
            t = 1.0f;
        }
        // The direction is kept; each axis is then clipped to the N64 stick's reach, as its gate does.
        scale = apply_curve(t) * STICK_EDGE / distance;
        x = clamp((int) lrintf(rawX * scale), -STICK_MAX, STICK_MAX);
        y = clamp((int) lrintf(rawY * scale), -STICK_MAX, STICK_MAX);
    }
    sLastOut[0] = x;
    sLastOut[1] = y;
    *outX = (signed char) x;
    *outY = (signed char) y;
}

int controls_tilt_enabled(void) {
    return sSettings.tilt != TILT_OFF;
}

void controls_apply_tilt(float roll, signed char *outX) {
    float t;
    int x;
    if (sSettings.tilt == TILT_OFF) {
        return;
    }
    if (sSettings.tilt == TILT_INVERTED) {
        roll = -roll;
    }
    if (roll > -TILT_DEADZONE && roll < TILT_DEADZONE) {
        return;
    }
    // Linear past the small deadzone, reaching the N64 stick's edge at the range; added to the stick's own steering.
    t = (roll - (roll > 0.0f ? TILT_DEADZONE : -TILT_DEADZONE)) / (sSettings.tiltRange - TILT_DEADZONE);
    x = clamp(*outX + (int) lrintf(t * STICK_EDGE), -STICK_MAX, STICK_MAX);
    sLastOut[0] = x;
    *outX = (signed char) x;
}

void controls_last_stick(int *rawX, int *rawY, int *outX, int *outY) {
    *rawX = sLastRaw[0];
    *rawY = sLastRaw[1];
    *outX = sLastOut[0];
    *outY = sLastOut[1];
}

void controls_keys_for(unsigned short actions, char *out, int size) {
    // Before controls.txt is read (by the input thread), the default layout.
    const unsigned short *map = sLoaded ? sSettings.map : sLayouts[0].map;
    int key, length = 0;
    out[0] = '\0';
    for (key = 0; key < CTRL_KEY_COUNT && size > 0; key++) {
        if (map[key] & actions) {
            length += snprintf(out + length, size - length, "%s%s", length > 0 ? " / " : "", sKeyNames[key]);
            if (length >= size) {
                break;
            }
        }
    }
}

/* Tilt steering needs the 3DS's accelerometer: elsewhere its two rows are left out of the list. */
#ifdef __3DS__
#define TILT_ROWS 0
#else
#define TILT_ROWS 2
#endif

/* The option (OPT_*) the list's row index stands for. */
static int option_at(int index) {
    return index >= OPT_TILT ? index + TILT_ROWS : index;
}

int controls_option_count(void) {
    return OPT_COUNT - TILT_ROWS;
}

const char *controls_option_label(int index) {
    static const char *const sKeyLabels[CTRL_KEY_COUNT] = { "A button",  "B button",  "X button",
                                                            "Y button",  "L button",  "R button",
                                                            "ZL button", "ZR button", "SELECT button" };
    index = option_at(index);
    if (index >= OPT_KEY_FIRST && index < OPT_KEY_FIRST + CTRL_KEY_COUNT) {
        return sKeyLabels[index - OPT_KEY_FIRST];
    }
    switch (index) {
        case OPT_LAYOUT:
            return "Button layout";
        case OPT_DEADZONE:
            return "Stick deadzone";
        case OPT_SENSITIVITY:
            return "Stick sensitivity";
        case OPT_CURVE:
            return "Stick response";
        case OPT_TILT:
            return "Tilt steering";
        case OPT_TILT_RANGE:
            return "Tilt for full lock";
        case OPT_RESET:
            return "Reset controls";
        default:
            return "";
    }
}

void controls_option_value(int index, char *out, int size) {
    int layout;
    controls_init();
    index = option_at(index);
    if (index >= OPT_KEY_FIRST && index < OPT_KEY_FIRST + CTRL_KEY_COUNT) {
        plat_snprintf(out, size, "%s", sActions[action_index(sSettings.map[index - OPT_KEY_FIRST])].label);
        return;
    }
    switch (index) {
        case OPT_LAYOUT:
            layout = current_layout();
            plat_snprintf(out, size, "%s", layout >= 0 ? sLayouts[layout].label : "Custom");
            break;
        case OPT_DEADZONE:
            plat_snprintf(out, size, "%d%%", sSettings.deadzone);
            break;
        case OPT_SENSITIVITY:
            plat_snprintf(out, size, "%d%%", sSettings.sensitivity);
            break;
        case OPT_CURVE:
            plat_snprintf(out, size, "%s", sCurveNames[sSettings.curve]);
            break;
        case OPT_TILT:
            plat_snprintf(out, size, "%s", sTiltNames[sSettings.tilt]);
            break;
        case OPT_TILT_RANGE:
            plat_snprintf(out, size, port_text("%d degrees"), sSettings.tiltRange);
            break;
        case OPT_RESET:
            // An action row: no value.
            if (size > 0) {
                out[0] = '\0';
            }
            break;
        default:
            if (size > 0) {
                out[0] = '\0';
            }
            break;
    }
}

/* Steps v by step within lo..hi (in units of unit), wrapping round at either end. */
static int wrap_step(int v, int step, int lo, int hi, int unit) {
    v += step * unit;
    if (v > hi) {
        return lo;
    }
    if (v < lo) {
        return hi;
    }
    return v;
}

/* The game's CONTROL OPTIONS (game thread) and the bottom screen's Controls page (its own thread) both change them. */
static volatile int sBusy;

void controls_option_change(int index, int step) {
    int count, layout;

    controls_init();
    while (__atomic_exchange_n(&sBusy, 1, __ATOMIC_ACQUIRE)) {
        plat_sleep_ns(1000 * 1000);
    }
    step = step < 0 ? -1 : 1;
    index = option_at(index);
    if (index >= OPT_KEY_FIRST && index < OPT_KEY_FIRST + CTRL_KEY_COUNT) {
        unsigned short *slot = &sSettings.map[index - OPT_KEY_FIRST];
        int a = action_index(*slot), tries;
        for (tries = 0; tries < ACTION_COUNT; tries++) {
            a = wrap_step(a, step, 0, ACTION_COUNT - 1, 1);
            if (action_offered(a)) {
                break;
            }
        }
        *slot = sActions[a].n64;
    } else {
        switch (index) {
            case OPT_LAYOUT:
                // From a custom mapping, the first step goes to the standard layout.
                layout = current_layout();
                count = LAYOUT_COUNT;
                layout = layout < 0 ? 0 : wrap_step(layout, step, 0, count - 1, 1);
                set_map(sLayouts[layout].map);
                break;
            case OPT_DEADZONE:
                sSettings.deadzone = wrap_step(sSettings.deadzone, step, 0, DEADZONE_MAX, DEADZONE_STEP);
                break;
            case OPT_SENSITIVITY:
                sSettings.sensitivity =
                    wrap_step(sSettings.sensitivity, step, SENSITIVITY_MIN, SENSITIVITY_MAX, SENSITIVITY_STEP);
                break;
            case OPT_CURVE:
                sSettings.curve = wrap_step(sSettings.curve, step, 0, CURVE_COUNT - 1, 1);
                break;
            case OPT_TILT:
                sSettings.tilt = wrap_step(sSettings.tilt, step, 0, TILT_COUNT - 1, 1);
                break;
            case OPT_TILT_RANGE:
                sSettings.tiltRange =
                    wrap_step(sSettings.tiltRange, step, TILT_RANGE_MIN, TILT_RANGE_MAX, TILT_RANGE_STEP);
                break;
            case OPT_RESET:
                set_defaults();
                break;
            default:
                __atomic_store_n(&sBusy, 0, __ATOMIC_RELEASE);
                return;
        }
    }
    save();
    __atomic_store_n(&sBusy, 0, __ATOMIC_RELEASE);
}
