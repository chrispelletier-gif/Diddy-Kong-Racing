#ifndef PORT_3DS_MENU_3DS_H
#define PORT_3DS_MENU_3DS_H

/* The Settings menu on the bottom screen (menu_3ds.c). */

/* What the panel (update_3ds.c) should do after a tap on the menu. */
enum { MENU_STAY, MENU_CLOSE, MENU_UPDATES, MENU_BUGREPORT };

/*
 * Adds a page, a tab of the menu called title. count() is its number of rows; label(i) and
 * value(i) what row i shows (an empty value: a row that does something when tapped, with no value of its own);
 * change(i, step) is called when it is tapped, step -1 on the "<" half of the value and +1 elsewhere. All are called
 * from the bottom screen's thread. Call it at start-up (before update_3ds_start).
 */
void menu_3ds_add_page(const char *title, int (*count)(void), const char *(*label)(int i),
                       void (*value)(int i, char *out, int size), void (*change)(int i, int step));

/* Adds the video settings to the game's own menus (plat/options.h). */
void menu_3ds_add_options(void);

void menu_3ds_open(void);
int menu_3ds_open_page(const char *title);
void menu_3ds_close(void);
int menu_3ds_can_scroll(int direction);
void menu_3ds_scroll(int direction);
int menu_3ds_tap(int x, int y);
void menu_3ds_refresh(void);

#endif
