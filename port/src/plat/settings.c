/* settings.txt (settings.h). Lines it doesn't know, comments aside, are kept when it is written again. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "recorder.h"
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

/*
 * The performance switches only a debug build's menus show (plat.h PLAT_DEBUG_TOOLS). A player's build keeps them at
 * their defaults, even if settings.txt was written by a debug build.
 */
static int is_debug_switch(const char *key) {
    static const char *const sKeys[] = { "renderahead", "guardband",  "sound_dsp",   "sound_core",
                                         "sound_rate",  "sound_buffer", "o3ds_render" };
    size_t i;
    for (i = 0; i < sizeof(sKeys) / sizeof(sKeys[0]); i++) {
        if (strcmp(key, sKeys[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

int settings_get(const char *key, int fallback) {
    Setting *setting;
    int value;
    if (!PLAT_DEBUG_TOOLS && is_debug_switch(key)) {
        return fallback;
    }
    lock();
    setting = find(key);
    value = setting != NULL ? setting->value : fallback;
    unlock();
    return value;
}

static int sDeferred;       // Writes held back (settings_defer): the first-run setup changes several settings a move.
static int sDeferredDirty;  // Something changed while they were held back.

/* Writes the file from the table; the caller holds the lock. */
static void write_file(void) {
    static char text[MAX_SETTINGS * 48];
    char path[256];
    int length, i;

    length = snprintf(text, sizeof(text), "# Written by the Settings menus (the game's Options and pause menus, the 3DS bottom screen).\n");
    for (i = 0; i < sCount; i++) {
        length += snprintf(text + length, sizeof(text) - length, "%s=%d\n", sSettings[i].key, sSettings[i].value);
    }
    file_path(path, sizeof(path));
    if (!plat_save_file(path, text, length)) {
        plat_log("settings: could not write %s", path);
    }
}

/* While on, settings_set only changes the table; turning it off writes the file once if anything changed. Every write
 * is a temporary file, a remove and a rename on the SD card, a noticeable hitch on the game's thread. */
void settings_defer(int on) {
    lock();
    sDeferred = on;
    if (!on && sDeferredDirty) {
        sDeferredDirty = 0;
        write_file();
    }
    unlock();
}

void settings_set(const char *key, int value) {
    Setting *setting;

    lock();
    setting = find(key);
    if (setting == NULL) {
        if (sCount >= MAX_SETTINGS || strlen(key) >= sizeof(sSettings[0].key)) {
            unlock();
            return;
        }
        setting = &sSettings[sCount++];
        snprintf(setting->key, sizeof(setting->key), "%s", key);
        rec_event("setting %s = %d (new)", key, value);
    } else if (setting->value == value) {
        unlock(); // Nothing to write.
        return;
    } else {
        rec_event("setting %s = %d (was %d)", key, value, setting->value);
    }
    setting->value = value;
    if (sDeferred) {
        sDeferredDirty = 1;
    } else {
        write_file();
    }
    unlock();
}

void settings_describe(char *out, int size) {
    int i, length = 0;
    if (size <= 0) {
        return;
    }
    out[0] = '\0';
    lock();
    if (sCount < 0) {
        unlock();
        settings_get("", 0); // Loads the file.
        lock();
    }
    for (i = 0; i < sCount && length < size - 1; i++) {
        length += snprintf(out + length, size - length, "%s%s=%d", i > 0 ? ", " : "", sSettings[i].key,
                           sSettings[i].value);
    }
    unlock();
}
