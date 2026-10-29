/*
 * The intro logos (port.h, roadmap Q10): the Rareware and N64 logo screen that plays before the title screen. The
 * Intro logos setting (Settings > Start-up, a row of the game's Options menu, skipintro= in settings.txt;
 * DKR_SKIP_INTRO in env.txt for a test) leaves it out, so the game goes from the boot screen straight to the title
 * screen. It is off, the N64's sequence, by default. Pressing A, B or START on the logo screen skips it either way
 * (src/menu.c).
 */
#include <stdlib.h>

#include "plat/plat.h"
#include "plat/settings.h"
#include "port.h"

static volatile s32 sSkipIntro = -1; // -1 until loaded.

s32 port_skip_intro(void) {
    if (sSkipIntro < 0) {
        const char *env = plat_getenv("DKR_SKIP_INTRO");
        sSkipIntro = env != NULL ? atoi(env) != 0 : settings_get("skipintro", 0) != 0;
    }
    return sSkipIntro;
}

void port_set_skip_intro(s32 on) {
    sSkipIntro = on != 0;
    settings_set("skipintro", sSkipIntro);
}

/* The Start-up page of the Settings menu (menu_3ds_add_page on the 3DS) and its row on the game's Options menu. */
int intro_option_count(void) {
    return 1;
}

const char *intro_option_label(int index) {
    (void) index;
    return "Intro logos";
}

void intro_option_value(int index, char *out, int size) {
    (void) index;
    plat_snprintf(out, size, "%s", port_skip_intro() ? "Skip" : "Show");
}

void intro_option_change(int index, int step) {
    (void) index;
    (void) step;
    port_set_skip_intro(!port_skip_intro());
}

const char *intro_option_hint(int index) {
    (void) index;
    return port_skip_intro() ? "Start at the title screen" : "A, B or START skips them";
}

/* Only the Options menu shows it: it is used at the next start. */
int intro_option_in_race(int index) {
    (void) index;
    return 0;
}
