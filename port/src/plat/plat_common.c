/* Platform helpers shared by every target (plain C library). */
#include "plat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

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

const char *plat_getenv(const char *name) {
#ifdef __3DS__
    (void) name;
    return NULL;
#else
    return getenv(name);
#endif
}
