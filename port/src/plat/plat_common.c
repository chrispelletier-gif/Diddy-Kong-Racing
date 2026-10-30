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

/* plat_save_file's temporary copy of a file. */
static void temp_path(char *out, size_t size, const char *path) {
    snprintf(out, size, "%s.tmp", path);
}

/* Moves the finished temporary copy over the file. The 3DS's SD card driver won't rename over a file that exists. */
static int replace_with_temp(const char *temp, const char *path) {
    if (rename(temp, path) == 0) {
        return 1;
    }
    remove(path);
    return rename(temp, path) == 0;
}

/* Opens a file, first finishing a save that stopped between removing the old file and renaming the new one in. */
static FILE *open_for_reading(const char *path) {
    FILE *f = fopen(path, "rb");
    char temp[300];

    if (f == NULL) {
        temp_path(temp, sizeof(temp), path);
        if (rename(temp, path) == 0) {
            f = fopen(path, "rb");
        }
    }
    return f;
}

void *plat_load_file(const char *path, unsigned int *size) {
    FILE *f = open_for_reading(path);
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
    FILE *f = open_for_reading(path);
    size_t got;

    if (f == NULL) {
        return -1;
    }
    got = fread(data, 1, size, f);
    fclose(f);
    return (int) got;
}

/*
 * Written to a temporary file which then replaces the old one, so that losing power or crashing part way leaves either
 * the old file or the new one, never a cut-off one. Returns 0 if anything failed (the old file is then kept).
 */
int plat_save_file(const char *path, const void *data, unsigned int size) {
    char temp[300];
    FILE *f;
    int ok;

    temp_path(temp, sizeof(temp), path);
    f = fopen(temp, "wb");
    if (f == NULL) {
        return 0;
    }
    ok = fwrite(data, 1, size, f) == size;
    ok = fflush(f) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok || !replace_with_temp(temp, path)) {
        remove(temp);
        return 0;
    }
    return 1;
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

#define MAX_SWITCH_OVERRIDES 16

static volatile struct {
    const char *volatile name; // NULL: a free entry
    int value;
} sSwitchOverrides[MAX_SWITCH_OVERRIDES];
volatile int gPlatSwitchEpoch;

int plat_switch(const char *name, int fallback) {
    const char *env;
    int i;
    for (i = 0; i < MAX_SWITCH_OVERRIDES; i++) {
        const char *entry = sSwitchOverrides[i].name;
        if (entry != NULL && strcmp(entry, name) == 0) {
            __sync_synchronize(); // Pairs with plat_override_switch's: the value is read after the name.
            return sSwitchOverrides[i].value;
        }
    }
    env = plat_getenv(name);
    return env != NULL ? atoi(env) : fallback;
}

void plat_override_switch(const char *name, int value) {
    int i, free = -1;
    for (i = 0; i < MAX_SWITCH_OVERRIDES; i++) {
        const char *entry = sSwitchOverrides[i].name;
        if (entry != NULL && strcmp(entry, name) == 0) {
            break;
        }
        if (sSwitchOverrides[i].name == NULL && free < 0) {
            free = i;
        }
    }
    if (i == MAX_SWITCH_OVERRIDES) {
        i = free;
    }
    if (i < 0) {
        return;
    }
    // The value before the name, as the renderer may be reading the table on the other core.
    sSwitchOverrides[i].value = value;
    __sync_synchronize();
    sSwitchOverrides[i].name = value < 0 ? NULL : name;
    gPlatSwitchEpoch++;
}

void plat_clear_switch_overrides(void) {
    int i;
    for (i = 0; i < MAX_SWITCH_OVERRIDES; i++) {
        sSwitchOverrides[i].name = NULL;
    }
    gPlatSwitchEpoch++;
}

int plat_list_dir(const char *dir, char *names, int nameSize, int maxNames) {
    return plat_list_dir_prefix(dir, "", names, nameSize, maxNames);
}

int plat_list_dir_prefix(const char *dir, const char *prefix, char *names, int nameSize, int maxNames) {
    DIR *d = opendir(dir);
    struct dirent *entry;
    size_t prefixLength = strlen(prefix);
    int count = 0;

    if (d == NULL) {
        return 0;
    }
    while (count < maxNames && (entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.' || (int) strlen(entry->d_name) >= nameSize ||
            strncmp(entry->d_name, prefix, prefixLength) != 0) {
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

const char *plat_version(void) {
#ifdef PORT_VERSION
    return PORT_VERSION;
#else
    return "host";
#endif
}

/* The render ahead setting (see plat.h): -1 until first read. */
static volatile int sRenderAhead = -1;

int plat_render_ahead(void) {
    if (sRenderAhead < 0) {
        const char *env = plat_getenv("DKR_RENDER_AHEAD");
        // 1 everywhere. An Old 3DS rendered ahead by 2 while its renderer and game took turns on one core (switch
        // benches #244, #245); with the two-core renderer, 1 kept 97% of frames at 30 like 2 did (#266), a frame
        // sooner.
        int frames = settings_get("renderahead", env != NULL ? atoi(env) : 1);
        sRenderAhead = frames == 2 ? 2 : 1;
        plat_log("render ahead: %d frame%s", sRenderAhead, sRenderAhead == 1 ? "" : "s");
    }
    return plat_switch("BENCH_RENDER_AHEAD", sRenderAhead); // The switch bench may set its own
}

// Per thread: the walk sets it for the frame it walks, the two-core renderer's second thread for the frame it ends.
static __thread int sFrameStep;

void plat_set_frame_step(int retraces) {
    sFrameStep = retraces;
}

int plat_frame_step(void) {
    return sFrameStep;
}

void plat_set_render_ahead(int frames) {
    sRenderAhead = frames == 2 ? 2 : 1;
    settings_set("renderahead", sRenderAhead);
}

/* The other ports survey's switches (see plat.h): -1 until first read. */
static volatile int sGpuMaths = -1, sStereoOnce = -1;

int plat_gpu_maths_setting(void) {
    if (sGpuMaths < 0) {
        sGpuMaths = settings_get("gpumaths", GFX_SURVEY_DEFAULT ? 3 : 0) & 3;
    }
    return sGpuMaths;
}

void plat_set_gpu_maths_setting(int level) {
    sGpuMaths = level & 3;
    settings_set("gpumaths", sGpuMaths);
}

int plat_stereo_once_setting(void) {
    if (sStereoOnce < 0) {
        sStereoOnce = settings_get("stereoonce", GFX_SURVEY_DEFAULT) != 0;
    }
    return sStereoOnce;
}

void plat_set_stereo_once_setting(int on) {
    sStereoOnce = on != 0;
    settings_set("stereoonce", sStereoOnce);
}

/* The 60 fps setting (see plat_frame_retraces): -1 until first read. sFps60 is what frames use now. sFps20 is the
 * steady 20 fps cap (settings.txt "fps20", else DKR_20FPS) for an Old 3DS that can't keep 30: every frame then takes
 * three retraces instead of jumping between two and three; 60 fps wins over it. It was an Old 3DS's default until the
 * two-core renderer (P21, switch bench #263: 29 fps, 93% of frames at 30) made 30 the Old 3DS's target. */
static volatile int sFps60Setting = -1, sFps60, sFps20;

static void read_60fps_setting(void) {
    if (sFps60Setting < 0) {
        const char *env = plat_getenv("DKR_60FPS");
        const char *env20 = plat_getenv("DKR_20FPS");
        sFps60Setting = settings_get("fps60", env != NULL && atoi(env) != 0) != 0;
        sFps60 = sFps60Setting;
        sFps20 = settings_get("fps20", env20 != NULL && atoi(env20) != 0) != 0;
        plat_log("frame rate cap: %d fps", sFps60 ? 60 : sFps20 ? 20 : 30);
    }
}

int plat_frame_retraces(void) {
    int pinned = plat_switch("DKR_FPS_PIN", 0); // The switch bench's own frame rate (20, 30 or 60).
    read_60fps_setting();
    if (pinned == 20 || pinned == 30 || pinned == 60) {
        return 60 / pinned;
    }
    return sFps60 ? 1 : sFps20 ? 3 : 2;
}

int plat_frame_rate_setting(void) {
    read_60fps_setting();
    return sFps60Setting ? 60 : sFps20 ? 20 : 30;
}

void plat_set_frame_rate_setting(int fps) {
    read_60fps_setting();
    sFps20 = fps == 20;
    settings_set("fps20", sFps20);
    plat_set_60fps_setting(fps == 60);
}

void plat_set_60fps(int on) {
    read_60fps_setting();
    sFps60 = on < 0 ? sFps60Setting : on != 0;
}

int plat_60fps_setting(void) {
    read_60fps_setting();
    return sFps60Setting;
}

static int sHudEdges = -1;

int plat_hud_edges(void) {
    if (sHudEdges < 0) {
        const char *env = plat_getenv("DKR_HUD_EDGES");
        sHudEdges = settings_get("hudedges", env != NULL ? atoi(env) != 0 : 1) != 0;
    }
    return sHudEdges;
}

void plat_set_hud_edges(int on) {
    sHudEdges = on != 0;
    settings_set("hudedges", sHudEdges);
}

void plat_set_60fps_setting(int on) {
    read_60fps_setting();
    sFps60Setting = sFps60 = on != 0;
    settings_set("fps60", sFps60Setting);
}

float plat_camera_near(void) {
    return plat_aspect_scale() > 1.0f ? 2.5f : 10.0f;
}
