#ifndef PORT_PLAT_SETTINGS_H
#define PORT_PLAT_SETTINGS_H

/*
 * The player's settings, kept as NAME=VALUE lines in settings.txt in the data directory (sdmc:/3ds/dkr/ on the 3DS).
 * The bottom screen's Settings menu (port/3ds/menu_3ds.c) changes them; env.txt stays for debug switches.
 */

/* The saved value of key, or fallback if the file doesn't have one. */
int settings_get(const char *key, int fallback);
/* Changes key and saves the file. */
void settings_set(const char *key, int value);
/* Every saved setting as "key=value, key=value", for bug reports. */
void settings_describe(char *out, int size);
/* Holds settings_set's file writes back while on (one write when it goes off). */
void settings_defer(int on);

#endif
