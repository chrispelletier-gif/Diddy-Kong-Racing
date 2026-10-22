/* Platform helpers shared by every target (plain C library). */
#include "plat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

volatile unsigned int gPlatCounters[PLAT_COUNTER_COUNT];
const char *volatile gPlatGfxStage = "not started";
volatile int gPlatSnapshotRequested;
volatile int gPlatSnapshotSaved;

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
