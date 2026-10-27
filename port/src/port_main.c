/* Native port entry point and glue shared by every platform. */
#include <ultra64.h>
#include "types.h"
#include "port.h"
#include "plat/plat.h"
#include "main.h"
#include <string.h>

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
    if (fileSize < size) {
        plat_log("saves: %s is only %u bytes, the rest is blank", name, fileSize);
    }
    bzero(data, size);
    bcopy(file, data, (fileSize < size) ? fileSize : size);
    plat_free(file);
    return TRUE;
}

s32 port_save_exists(const char *name) {
    char path[256];
    u8 byte;
    make_path(path, sizeof(path), name);
    return plat_load_file_start(path, &byte, 1) >= 0;
}

s32 port_save_write(const char *name, const void *data, u32 size) {
    char path[256];
    make_path(path, sizeof(path), name);
    return plat_save_file(path, data, size);
}

static s32 has_rom_extension(const char *name) {
    s32 len = strlen(name);
    const char *ext;
    if (len < 4) {
        return FALSE;
    }
    ext = name + len - 4;
    return ext[0] == '.' && (ext[1] == 'z' || ext[1] == 'Z' || ext[1] == 'n' || ext[1] == 'N' || ext[1] == 'v' ||
                             ext[1] == 'V') &&
           ext[2] == '6' && ext[3] == '4';
}

/* Why the most telling ROM candidate was turned down (port_rom_problem() at the time). */
static char sRomProblem[320];

/*
 * Looks for the ROM: first under the expected names in the data directory, then any .z64/.n64/.v64 file in the
 * search directories. Returns PORT_ROM_OK, or the most informative failure seen, with its reason in sRomProblem.
 */
static s32 find_rom(char *foundPath, s32 pathSize) {
    static const char *romNames[] = { "baserom.z64", "baserom.n64", "baserom.v64", "dkr.z64", "dkr.n64", "dkr.v64" };
    static char names[64][128];
    const char *const *dirs = plat_rom_search_dirs();
    s32 best = PORT_ROM_NOT_FOUND;
    s32 result, numNames, d, i;

    for (i = 0; i < (s32) (sizeof(romNames) / sizeof(romNames[0])); i++) {
        make_path(foundPath, pathSize, romNames[i]);
        result = port_rom_load(foundPath);
        if (result == PORT_ROM_OK) {
            return result;
        }
        if (result < best) {
            best = result;
            plat_snprintf(sRomProblem, sizeof(sRomProblem), "%s", port_rom_problem());
        }
    }
    for (d = 0; dirs[d] != NULL; d++) {
        // The whole array, not &names[0][0]: the fortified strcpy would take that for a single 128-byte name.
        numNames = plat_list_dir(dirs[d], (char *) names, sizeof(names[0]), 64);
        for (i = 0; i < numNames; i++) {
            if (!has_rom_extension(names[i])) {
                continue;
            }
            plat_snprintf(foundPath, pathSize, "%s/%s", dirs[d], names[i]);
            result = port_rom_load(foundPath);
            port_log("ROM candidate %s: %d", foundPath, (int) result);
            if (result == PORT_ROM_OK) {
                return result;
            }
            if (result < best) {
                best = result;
                plat_snprintf(sRomProblem, sizeof(sRomProblem), "%s", port_rom_problem());
            }
        }
    }
    return best;
}

/* Loads the ROM and boots the game. Returns once the game threads are running. */
void port_boot(void) {
    char path[256];
    s32 result;

    unsigned long long start;

    plat_init();
    plat_status("Loading the ROM...");
    start = plat_time_ns();
    result = find_rom(path, sizeof(path));
    switch (result) {
        case PORT_ROM_OK:
            plat_status("ROM loaded in %.1f s", (plat_time_ns() - start) / 1e9);
            port_log("Using ROM %s", path);
            plat_rom_loaded();
            break;
        case PORT_ROM_WRONG_VERSION:
            port_fatal("Wrong version of the ROM.\n\n%s\n\nThis build plays the %s release only.", sRomProblem,
                       port_rom_supported_release());
            break;
        case PORT_ROM_DAMAGED:
            port_fatal("The ROM could not be used.\n\n%s\n\nUse an unmodified dump of the %s release.", sRomProblem,
                       port_rom_supported_release());
            break;
        case PORT_ROM_NOT_DKR:
        case PORT_ROM_NOT_N64:
            port_fatal("Could not find the Diddy Kong Racing ROM.\n\n%s\n\nCopy your %s ROM (.z64, .n64 or .v64)\nto "
                       "%s/\non the SD card, e.g. as baserom.z64.",
                       sRomProblem, port_rom_supported_release(), plat_data_dir());
            break;
        default:
            port_fatal("Could not find the Diddy Kong Racing ROM.\n\nCopy your %s ROM (.z64, .n64 or .v64)\nto "
                       "%s/\non the SD card, e.g. as baserom.z64.",
                       port_rom_supported_release(), plat_data_dir());
            break;
    }
    port_si_init();
    port_vi_init();
    port_rcp_init();
    plat_status("Starting the game...");
    mainproc();
}
