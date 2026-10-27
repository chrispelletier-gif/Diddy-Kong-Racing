/*
 * Peripheral interface emulation: the cartridge ROM is read from a file supplied by the user and every PI DMA
 * becomes a memcpy out of it.
 */
#include <ultra64.h>
#include "common.h"
#include "types.h"
#include "port.h"
#include "plat/plat.h"

static u8 *sRomData;
static u32 sRomSize;
static u32 sAssetsLutStart;
static u32 sAssetsLutEnd;

/* ROM offset of the asset lookup table for each version (see tools/dkr_assets_tool_extract.json). */
#if defined(VERSION_us_v77)
#define ASSETS_LUT_ROM_OFFSET 969568
#elif defined(VERSION_us_v80)
#define ASSETS_LUT_ROM_OFFSET 970976
#elif defined(VERSION_pal_v77)
#define ASSETS_LUT_ROM_OFFSET 969712
#elif defined(VERSION_pal_v80)
#define ASSETS_LUT_ROM_OFFSET 971120
#elif defined(VERSION_jpn_v79)
#define ASSETS_LUT_ROM_OFFSET 976336
#else
#error "Unknown game version"
#endif

/* Converts .v64 (byte swapped) and .n64 (little endian) dumps to the native .z64 byte order. */
static s32 normalise_rom_byte_order(u8 *rom, u32 size) {
    u32 i;
    u8 tmp;

    if (size < 0x1000) {
        return FALSE;
    }
    if (rom[0] == 0x80 && rom[1] == 0x37) {
        return TRUE; // .z64
    }
    if (rom[0] == 0x37 && rom[1] == 0x80) {
        for (i = 0; i + 1 < size; i += 2) {
            tmp = rom[i];
            rom[i] = rom[i + 1];
            rom[i + 1] = tmp;
        }
        return TRUE;
    }
    if (rom[0] == 0x40 && rom[1] == 0x12) {
        for (i = 0; i + 3 < size; i += 4) {
            tmp = rom[i];
            rom[i] = rom[i + 3];
            rom[i + 3] = tmp;
            tmp = rom[i + 1];
            rom[i + 1] = rom[i + 2];
            rom[i + 2] = tmp;
        }
        return TRUE;
    }
    return FALSE;
}

/* The releases of the game, told apart by the checksums in the ROM header (see src/hasm/header.s). */
static const struct {
    u32 crc1, crc2;
    const char *name;
} sReleases[] = {
    { 0x53D440E7, 0x7519B011, "US 1.0" },  { 0xE402430D, 0xD2FCFC9D, "US 1.1" },
    { 0xFD73F775, 0x9724755A, "PAL 1.0" }, { 0x596E145B, 0xF7D9879F, "PAL 1.1" },
    { 0x7435C9BB, 0x39763CF4, "Japanese" },
};

/* The release this build is for, as its index in sReleases and its country code and revision in the header. */
#if defined(VERSION_us_v77)
#define ROM_RELEASE 0
#define ROM_COUNTRY 'E'
#define ROM_REVISION 0
#elif defined(VERSION_us_v80)
#define ROM_RELEASE 1
#define ROM_COUNTRY 'E'
#define ROM_REVISION 1
#elif defined(VERSION_pal_v77)
#define ROM_RELEASE 2
#define ROM_COUNTRY 'P'
#define ROM_REVISION 0
#elif defined(VERSION_pal_v80)
#define ROM_RELEASE 3
#define ROM_COUNTRY 'P'
#define ROM_REVISION 1
#else
#define ROM_RELEASE 4
#define ROM_COUNTRY 'J'
#define ROM_REVISION 0
#endif

/* Why the last ROM that port_rom_load turned down was not used, for the message on screen. */
static char sRomProblem[320];

const char *port_rom_supported_release(void) {
    return sReleases[ROM_RELEASE].name;
}

const char *port_rom_problem(void) {
    return sRomProblem;
}

/* The release a header belongs to (an index in sReleases), or -1 if its checksums match none of them. */
static s32 find_release(const u8 *header) {
    u32 crc1 = port_read_be32(header + 0x10), crc2 = port_read_be32(header + 0x14);
    s32 i;
    for (i = 0; i < (s32) (sizeof(sReleases) / sizeof(sReleases[0])); i++) {
        if (sReleases[i].crc1 == crc1 && sReleases[i].crc2 == crc2) {
            return i;
        }
    }
    return -1;
}

/*
 * Checks the header first, so that other ROMs on the SD card are not read in full: the internal name, then which
 * release it is. A header whose checksums match no release (a ROM hack, a translation or a bad dump) is let through if
 * its country code and revision are right; the asset table check in port_rom_load then decides.
 */
static s32 check_header(const char *path) {
    static u8 header[0x1000];
    char name[21];
    s32 got = plat_load_file_start(path, header, sizeof(header));
    s32 release, i;

    if (got < 0) {
        return PORT_ROM_NOT_FOUND;
    }
    if (got != (s32) sizeof(header) || !normalise_rom_byte_order(header, sizeof(header))) {
        plat_snprintf(sRomProblem, sizeof(sRomProblem), "%s is not an N64 ROM.", path);
        return PORT_ROM_NOT_N64;
    }
    if (bcmp(header + 0x20, "Diddy Kong Racing", 17) != 0) {
        for (i = 0; i < 20; i++) {
            u8 c = header[0x20 + i];
            name[i] = (c >= 0x20 && c < 0x7F) ? c : ' ';
        }
        for (name[20] = '\0'; i > 0 && name[i - 1] == ' '; i--) {
            name[i - 1] = '\0';
        }
        plat_snprintf(sRomProblem, sizeof(sRomProblem), "%s is %s%s%s, not Diddy Kong Racing.", path,
                      name[0] != '\0' ? "\"" : "another game", name, name[0] != '\0' ? "\"" : "");
        return PORT_ROM_NOT_DKR;
    }
    release = find_release(header);
    if (release >= 0 && release != ROM_RELEASE) {
        plat_snprintf(sRomProblem, sizeof(sRomProblem), "%s is the %s release.", path, sReleases[release].name);
        return PORT_ROM_WRONG_VERSION;
    }
    if (release < 0 && (header[0x3E] != ROM_COUNTRY || (header[0x3F] & 0xF) != ROM_REVISION)) {
        plat_snprintf(sRomProblem, sizeof(sRomProblem), "%s is a release of the game this build can't use%s.", path,
                      header[0x3E] == 'E' ? " (US)" : header[0x3E] == 'P' ? " (PAL)" : header[0x3E] == 'J' ? " (JP)" : "");
        return PORT_ROM_WRONG_VERSION;
    }
    if (release < 0) {
        port_log("%s: header checksums %08X %08X match no release (modified ROM?)", path,
                 (unsigned) port_read_be32(header + 0x10), (unsigned) port_read_be32(header + 0x14));
    }
    return PORT_ROM_OK;
}

s32 port_rom_load(const char *path) {
    unsigned int size;
    u32 count, lutStart, lutEnd;
    u8 *data;
    s32 headerResult = check_header(path);

    if (headerResult != PORT_ROM_OK) {
        return headerResult;
    }
    data = plat_load_file(path, &size);
    if (data == NULL) {
        plat_snprintf(sRomProblem, sizeof(sRomProblem), "%s could not be read (not enough memory?).", path);
        return PORT_ROM_DAMAGED;
    }
    if (!normalise_rom_byte_order(data, size)) {
        plat_free(data);
        plat_snprintf(sRomProblem, sizeof(sRomProblem), "%s is not an N64 ROM.", path);
        return PORT_ROM_NOT_N64;
    }
    // The table starts with the number of sections and is followed by (count + 1) offsets. If it isn't where this
    // release keeps it, or doesn't fit in the file, the ROM was changed or cut short.
    lutStart = ASSETS_LUT_ROM_OFFSET;
    count = (lutStart + 4 <= size) ? port_read_be32(data + lutStart) : 0;
    lutEnd = lutStart + (count + 2) * 4;
    if (count < 40 || count > 64 || lutEnd > size || port_read_be32(data + lutEnd - 4) > size - lutEnd) {
        plat_free(data);
        plat_snprintf(sRomProblem, sizeof(sRomProblem),
                      "%s is Diddy Kong Racing, but looks modified or damaged (%u KB).", path, size / 1024);
        return PORT_ROM_DAMAGED;
    }
    sRomData = data;
    sRomSize = size;
    sAssetsLutStart = lutStart;
    sAssetsLutEnd = lutEnd;
    return PORT_ROM_OK;
}

const u8 *port_rom_data(void) {
    return sRomData;
}

u32 port_rom_size(void) {
    return sRomSize;
}

u32 port_assets_lut_start(void) {
    return sAssetsLutStart;
}

u32 port_assets_lut_end(void) {
    return sAssetsLutEnd;
}

void port_rom_read(u32 romOffset, void *dst, u32 size) {
    // Cartridge addresses may come in as physical bus addresses.
    romOffset &= 0x0FFFFFFF;
    if (romOffset >= sRomSize) {
        bzero(dst, size);
        return;
    }
    // Compared without adding, which could wrap round for a huge size and skip the clamp.
    if (size > sRomSize - romOffset) {
        bzero((u8 *) dst + (sRomSize - romOffset), size - (sRomSize - romOffset));
        size = sRomSize - romOffset;
    }
    bcopy(sRomData + romOffset, dst, size);
}

/******************************** libultra API ********************************/

OSPiHandle *__osPiTable = NULL;
static OSPiHandle sCartHandle;

void osCreatePiManager(OSPri pri, OSMesgQueue *cmdQ, OSMesg *cmdBuf, s32 cmdMsgCnt) {
    (void) pri;
    (void) cmdQ;
    (void) cmdBuf;
    (void) cmdMsgCnt;
}

s32 osPiStartDma(OSIoMesg *mb, s32 priority, s32 direction, u32 devAddr, void *vAddr, u32 nbytes,
                 OSMesgQueue *mq) {
    (void) priority;
    if (direction == OS_READ) {
        port_rom_read(devAddr, vAddr, nbytes);
    }
    mb->hdr.retQueue = mq;
    mb->dramAddr = vAddr;
    mb->devAddr = devAddr;
    mb->size = nbytes;
    port_post_mesg(mq, mb);
    port_lockstep_dma(nbytes);
    return 0;
}

s32 osEPiStartDma(OSPiHandle *pihandle, OSIoMesg *mb, s32 direction) {
    (void) pihandle;
    return osPiStartDma(mb, mb->hdr.pri, direction, mb->devAddr, mb->dramAddr, mb->size, mb->hdr.retQueue);
}

s32 osPiRawStartDma(s32 direction, u32 devAddr, void *dramAddr, u32 size) {
    if (direction == OS_READ) {
        port_rom_read(devAddr, dramAddr, size);
    }
    return 0;
}

s32 osPiReadIo(u32 devAddr, u32 *data) {
    *data = port_read_be32(sRomData + (devAddr & 0x0FFFFFFF));
    return 0;
}

OSPiHandle *osCartRomInit(void) {
    sCartHandle.type = DEVICE_TYPE_CART;
    sCartHandle.baseAddress = PHYS_TO_K1(PI_DOM1_ADDR2);
    return &sCartHandle;
}

u32 osPiGetStatus(void) {
    return 0;
}
