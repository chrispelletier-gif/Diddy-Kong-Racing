/* Native port entry point and glue shared by every platform. */
#include <ultra64.h>
#include "types.h"
#include "port.h"
#include "plat/plat.h"
#include "main.h"

/* The game's whole heap. On the N64 this is the RAM between the end of BSS and the end of RDRAM. */
u8 gPortMainPool[PORT_MAIN_POOL_SIZE] __attribute__((aligned(64)));

/* Microcode symbols. Only their addresses are used, to fill in OSTask structures. */
long long int rspbootTextStart[1], rspbootTextEnd[1];
long long int rspF3DDKRXbusStart[1], rspF3DDKRDataXbusStart[1];
long long int rspF3DDKRFifoStart[1], rspF3DDKRDataFifoStart[1];
long long int aspMainTextStart[1], aspMainDataStart[1];

/* The C version of the game's inflate replaces this routine (see port/src/game/inflate.c). */
s32 gzip_inflate_block(void) {
    return 0;
}

void port_swap16_array(void *ptr, s32 count) {
    u16 *p = (u16 *) ptr;
    while (count-- > 0) {
        *p = PORT_BSWAP16(*p);
        p++;
    }
}

void port_swap32_array(void *ptr, s32 count) {
    u32 *p = (u32 *) ptr;
    while (count-- > 0) {
        *p = PORT_BSWAP32(*p);
        p++;
    }
}

static void make_path(char *out, s32 size, const char *name) {
    plat_snprintf(out, size, "%s/%s", plat_data_dir(), name);
}

s32 port_save_read(const char *name, void *data, u32 size) {
    char path[256];
    unsigned int fileSize;
    u8 *file;

    make_path(path, sizeof(path), name);
    file = plat_load_file(path, &fileSize);
    if (file == NULL) {
        return FALSE;
    }
    bzero(data, size);
    bcopy(file, data, (fileSize < size) ? fileSize : size);
    plat_free(file);
    return TRUE;
}

s32 port_save_write(const char *name, const void *data, u32 size) {
    char path[256];
    make_path(path, sizeof(path), name);
    return plat_save_file(path, data, size);
}

/* Loads the ROM and boots the game. Returns once the game threads are running. */
void port_boot(void) {
    static const char *romNames[] = { "baserom.z64", "baserom.n64", "baserom.v64", "dkr.z64", "dkr.n64", "dkr.v64" };
    char path[256];
    u32 i;
    s32 loaded = FALSE;

    plat_init();
    for (i = 0; i < sizeof(romNames) / sizeof(romNames[0]) && !loaded; i++) {
        make_path(path, sizeof(path), romNames[i]);
        loaded = port_rom_load(path);
    }
    if (!loaded) {
        port_fatal("Could not find the Diddy Kong Racing ROM.\nPlace it at %s/baserom.z64", plat_data_dir());
    }
    port_si_init();
    port_vi_init();
    port_rcp_init();
    mainproc();
}
