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

/* The Settings menu closes (the 3DS bottom screen's; nothing on the PC). */
void plat_request_close_menu(void);

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

/*
 * The Start-up page of the Settings menu (menu_3ds_add_page on the 3DS) and its rows (the intro logos row is also on the game's Options menu): the intro
 * logos, and the first-run setup (port_setup.c), which can be asked for again at the next start.
 */
int intro_option_count(void) {
    return 2;
}

const char *intro_option_label(int index) {
    return index == 0 ? "Intro logos" : "First-run setup";
}

void intro_option_value(int index, char *out, int size) {
    if (index == 0) {
        plat_snprintf(out, size, "%s", port_skip_intro() ? "Skip" : "Show");
    } else {
        plat_snprintf(out, size, "%s", port_setup_done() ? "Done" : "Next start");
    }
}

void intro_option_change(int index, int step) {
    (void) step;
    if (index == 0) {
        port_set_skip_intro(!port_skip_intro());
    } else if (PLAT_DEBUG_TOOLS) {
        port_setup_restart(); // Runs now at the title screen: no settings to delete.
        plat_request_close_menu();
    } else if (port_setup_done()) {
        port_setup_reset();
    } else {
        settings_set("setup", 1);
    }
}

const char *intro_option_hint(int index) {
    if (index == 1) {
        if (PLAT_DEBUG_TOOLS) {
            return "Runs now at the title screen";
        }
        return port_setup_done() ? "Language, play style and picture again" : "Shows at the title screen next start";
    }
    return port_skip_intro() ? "Start at the title screen" : "A, B or START skips them";
}

/* Only the Options menu shows them: they are used at the next start. */
int intro_option_in_race(int index) {
    (void) index;
    return 0;
}
