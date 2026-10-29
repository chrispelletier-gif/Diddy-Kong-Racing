#ifndef PORT_PLAT_OPTIONS_H
#define PORT_PLAT_OPTIONS_H

/*
 * The port's settings as the game's own menus show them (port/src/game/port_options_menu.c): each belongs to a
 * category, which the Options menu and the pause menu list as VIDEO OPTIONS, CAMERA OPTIONS and so on. The 3DS
 * bottom screen's Settings menu changes the same values through the same functions, so both share what is saved.
 *
 * Other parts of the port add a section of rows to a category at start-up with options_add; a category's rows are its
 * sections' rows in the order they were added. A section's functions are those of a bottom screen page
 * (menu_3ds_add_page): count, label, value, change (step -1 or +1), and optionally a hint for a row, and whether a
 * row may be changed from the pause menu (NULL: every row can).
 *
 * OPTIONS_GENERAL's rows have no page of their own: the Options menu shows each as a row of its own next to the game's
 * SUBTITLES ON, with its label and value ("INTRO LOGOS SHOW"), changed with left and right.
 */

enum {
    OPTIONS_VIDEO,
    OPTIONS_SCREEN,
    OPTIONS_CAMERA,
    OPTIONS_AUDIO,
    OPTIONS_CONTROLS,
    OPTIONS_GENERAL,
    OPTIONS_CATEGORY_COUNT
};

typedef struct OptionsSection {
    int (*count)(void);
    const char *(*label)(int i);
    void (*value)(int i, char *out, int size); /* An empty value: a row that does something when picked. */
    void (*change)(int i, int step);
    const char *(*hint)(int i); /* Optional. */
    int (*in_race)(int i);      /* Optional: 0 for a row only the Options menu shows (a change needing a restart). */
} OptionsSection;

void options_add(int category, const OptionsSection *section);

/* The rows of a category; inRace counts only the ones the pause menu shows. */
int options_count(int category, int inRace);
const char *options_label(int category, int inRace, int row);
void options_value(int category, int inRace, int row, char *out, int size);
const char *options_hint(int category, int inRace, int row);
void options_change(int category, int inRace, int row, int step);

/*
 * For a list of count rows some of which hidden(row) leaves out (a player's build hides the debug ones, plat.h
 * PLAT_DEBUG_TOOLS): the number of rows shown, and the row the i-th one shown is (-1 past the end).
 */
int options_visible_count(int count, int (*hidden)(int row));
int options_visible_row(int count, int (*hidden)(int row), int i);

/* Counts the changes made with options_change, so another menu showing the same values knows to redraw. */
unsigned int options_change_count(void);

#endif
