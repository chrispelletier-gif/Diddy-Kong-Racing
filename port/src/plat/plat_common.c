/* Platform helpers shared by every target (plain C library). */
#include "plat.h"
#include "settings.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

volatile unsigned int gPlatCounters[PLAT_COUNTER_COUNT];
const char *volatile gPlatGfxStage = "not started";
volatile int gPlatSnapshotRequested;
volatile int gPlatSnapshotSaved;
static volatile int sGamePauseRequested;

/* The game note, with a sequence number that is odd while it is being set: a reader that sees the same even number
 * before and after copying it has a whole note. */
static struct {
    volatile unsigned int seq;
    volatile short fields[sizeof(PlatGameNote) / sizeof(short)];
} sGameNote;

void plat_set_game_note(const PlatGameNote *note) {
    const short *in = (const short *) note;
    unsigned int i;
    sGameNote.seq++;
    __sync_synchronize();
    for (i = 0; i < sizeof(PlatGameNote) / sizeof(short); i++) {
        sGameNote.fields[i] = in[i];
    }
    __sync_synchronize();
    sGameNote.seq++;
}

int plat_get_game_note(PlatGameNote *out) {
    PlatGameNote copy;
    short *to = (short *) &copy;
    int tries;
    // Only a few tries: the game thread may be stopped halfway (on the same core, below the reader's priority).
    for (tries = 0; tries < 4; tries++) {
        unsigned int seq = sGameNote.seq, i;
        if (seq & 1) {
            continue;
        }
        __sync_synchronize();
        for (i = 0; i < sizeof(PlatGameNote) / sizeof(short); i++) {
            to[i] = sGameNote.fields[i];
        }
        __sync_synchronize();
        if (sGameNote.seq == seq) {
            *out = copy;
            return 1;
        }
    }
    return 0;
}

void plat_format_game_note(const PlatGameNote *note, char *out, int size) {
    if (!note->racing) {
        plat_snprintf(out, size, "not racing");
        return;
    }
    plat_snprintf(out, size, "lap %d cp %d bananas %d boost %d item %d/%d shield %d magnet %d hit %d", note->lap,
                  note->checkpoint, note->bananas, note->boost, note->itemType, note->itemCount, note->shield,
                  note->magnet, note->hit);
}

void plat_request_game_pause(void) {
    sGamePauseRequested = 1;
}

int plat_take_game_pause(void) {
    if (!sGamePauseRequested) {
        return 0;
    }
    sGamePauseRequested = 0;
    return 1;
}

static volatile int sUiSound;

void plat_request_ui_sound(int sound) {
    sUiSound = sound;
}

int plat_take_ui_sound(void) {
    return __atomic_exchange_n(&sUiSound, PLAT_UI_SOUND_NONE, __ATOMIC_ACQ_REL);
}

void *plat_load_file(const char *path, unsigned int *size) {
    FILE *f = fopen(path, "rb");
    long len;
    void *data;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return NULL;
    }
    data = malloc((size_t) len);
    if (data == NULL || fread(data, 1, (size_t) len, f) != (size_t) len) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (unsigned int) len;
    return data;
}

int plat_load_file_start(const char *path, void *data, unsigned int size) {
    FILE *f = fopen(path, "rb");
    size_t got;

    if (f == NULL) {
        return -1;
    }
    got = fread(data, 1, size, f);
    fclose(f);
    return (int) got;
}

int plat_save_file(const char *path, const void *data, unsigned int size) {
    FILE *f = fopen(path, "wb");
    size_t written;

    if (f == NULL) {
        return 0;
    }
    written = fwrite(data, 1, size, f);
    fclose(f);
    return written == size;
}

void *plat_malloc(size_t size) {
    return malloc(size);
}

void plat_free(void *ptr) {
    free(ptr);
}

int plat_snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    int ret;
    va_start(args, fmt);
    ret = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return ret;
}

void port_log(const char *fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    plat_log("%s", buffer);
}

void port_fatal(const char *fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    plat_fatal("%s", buffer);
}

#ifdef __3DS__
/* There is no environment on the 3DS, so the same settings can be given as NAME=VALUE lines in env.txt. */
const char *plat_getenv(const char *name) {
    static char *sEnvFile;
    static int sLoaded;
    size_t nameLen = strlen(name);
    char *line;

    if (!sLoaded) {
        char path[256];
        unsigned int size;
        sLoaded = 1;
        snprintf(path, sizeof(path), "%s/env.txt", plat_data_dir());
        sEnvFile = plat_load_file(path, &size);
        if (sEnvFile != NULL) {
            sEnvFile = realloc(sEnvFile, size + 2);
            sEnvFile[size] = 0;
            sEnvFile[size + 1] = 1; // Marks the end; lines are separated by NULs.
            for (line = sEnvFile; *line; line++) {
                if (*line == '\r' || *line == '\n') {
                    *line = 0;
                }
            }
        }
    }
    if (sEnvFile == NULL) {
        return NULL;
    }
    for (line = sEnvFile; *line != 1;) {
        if (strncmp(line, name, nameLen) == 0 && line[nameLen] == '=') {
            return line + nameLen + 1;
        }
        line += strlen(line) + 1;
    }
    return NULL;
}
#else
const char *plat_getenv(const char *name) {
    return getenv(name);
}
#endif

int plat_list_dir(const char *dir, char *names, int nameSize, int maxNames) {
    DIR *d = opendir(dir);
    struct dirent *entry;
    int count = 0;

    if (d == NULL) {
        return 0;
    }
    while (count < maxNames && (entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.' || (int) strlen(entry->d_name) >= nameSize) {
            continue;
        }
        strcpy(names + count * nameSize, entry->d_name);
        count++;
    }
    closedir(d);
    return count;
}

const char *plat_build_version(void) {
#ifdef PORT_BUILD_VERSION
    return PORT_BUILD_VERSION;
#else
    return "host";
#endif
}

/* The render ahead setting (see plat.h): -1 until first read. */
static volatile int sRenderAhead = -1;

int plat_render_ahead(void) {
    if (sRenderAhead < 0) {
        const char *env = plat_getenv("DKR_RENDER_AHEAD");
        int frames = settings_get("renderahead", env != NULL ? atoi(env) : 1);
        sRenderAhead = frames == 2 ? 2 : 1;
        plat_log("render ahead: %d frame%s", sRenderAhead, sRenderAhead == 1 ? "" : "s");
    }
    return sRenderAhead;
}

void plat_set_render_ahead(int frames) {
    sRenderAhead = frames == 2 ? 2 : 1;
    settings_set("renderahead", sRenderAhead);
}

/* The 60 fps setting (see plat_frame_retraces): -1 until first read. sFps60 is what frames use now. */
static volatile int sFps60Setting = -1, sFps60;

static void read_60fps_setting(void) {
    if (sFps60Setting < 0) {
        const char *env = plat_getenv("DKR_60FPS");
        sFps60Setting = settings_get("fps60", env != NULL && atoi(env) != 0) != 0;
        sFps60 = sFps60Setting;
        plat_log("frame rate cap: %d fps", sFps60 ? 60 : 30);
    }
}

int plat_frame_retraces(void) {
    read_60fps_setting();
    return sFps60 ? 1 : 2;
}

void plat_set_60fps(int on) {
    read_60fps_setting();
    sFps60 = on < 0 ? sFps60Setting : on != 0;
}

int plat_60fps_setting(void) {
    read_60fps_setting();
    return sFps60Setting;
}

void plat_set_60fps_setting(int on) {
    read_60fps_setting();
    sFps60Setting = sFps60 = on != 0;
    settings_set("fps60", sFps60Setting);
}
