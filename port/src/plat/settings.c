/* settings.txt (settings.h). Lines it doesn't know, comments aside, are kept when it is written again. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "settings.h"

#define MAX_SETTINGS 64

typedef struct Setting {
    char key[32];
    int value;
} Setting;

static Setting sSettings[MAX_SETTINGS];
static int sCount = -1;
// The game's menus (game thread) and the 3DS bottom screen's (its own thread) both change settings.
static volatile int sBusy;

static void lock(void) {
    while (__atomic_exchange_n(&sBusy, 1, __ATOMIC_ACQUIRE)) {
        plat_sleep_ns(1000 * 1000);
    }
}

static void unlock(void) {
    __atomic_store_n(&sBusy, 0, __ATOMIC_RELEASE);
}

static void file_path(char *path, size_t size) {
    snprintf(path, size, "%s/settings.txt", plat_data_dir());
}

static void load(void) {
    char path[256], *text, *line, *next;
    unsigned int size;

    sCount = 0;
    file_path(path, sizeof(path));
    text = plat_load_file(path, &size);
    if (text == NULL) {
        return;
    }
    text = realloc(text, size + 1);
    text[size] = '\0';
    for (line = text; line != NULL && *line != '\0' && sCount < MAX_SETTINGS; line = next) {
        char *value;
        next = strpbrk(line, "\r\n");
        if (next != NULL) {
            *next++ = '\0';
        }
        value = strchr(line, '=');
        if (*line == '#' || value == NULL || value == line || value - line >= (int) sizeof(sSettings[0].key)) {
            continue;
        }
        *value++ = '\0';
        snprintf(sSettings[sCount].key, sizeof(sSettings[0].key), "%s", line);
        sSettings[sCount].value = atoi(value);
        sCount++;
    }
    free(text);
}

static Setting *find(const char *key) {
    int i;
    if (sCount < 0) {
        load();
    }
    for (i = 0; i < sCount; i++) {
        if (strcmp(sSettings[i].key, key) == 0) {
            return &sSettings[i];
        }
    }
    return NULL;
}

int settings_get(const char *key, int fallback) {
    Setting *setting;
    int value;
    lock();
    setting = find(key);
    value = setting != NULL ? setting->value : fallback;
    unlock();
    return value;
}

void settings_set(const char *key, int value) {
    Setting *setting;
    static char text[MAX_SETTINGS * 48];
    char path[256];
    int length, i;

    lock();
    setting = find(key);
    if (setting == NULL) {
        if (sCount >= MAX_SETTINGS || strlen(key) >= sizeof(sSettings[0].key)) {
            unlock();
            return;
        }
        setting = &sSettings[sCount++];
        snprintf(setting->key, sizeof(setting->key), "%s", key);
    }
    setting->value = value;
    length = snprintf(text, sizeof(text), "# Written by the Settings menus (the game's Options and pause menus, the 3DS bottom screen).\n");
    for (i = 0; i < sCount; i++) {
        length += snprintf(text + length, sizeof(text) - length, "%s=%d\n", sSettings[i].key, sSettings[i].value);
    }
    file_path(path, sizeof(path));
    if (!plat_save_file(path, text, length)) {
        plat_log("settings: could not write %s", path);
    }
    unlock();
}
