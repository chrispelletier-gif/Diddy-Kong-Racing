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

/* Checks the internal name in the header first, so that other ROMs on the SD card are not read in full. */
static s32 is_dkr_header(const char *path) {
    static u8 header[0x1000];
    s32 got = plat_load_file_start(path, header, sizeof(header));

    if (got < 0) {
        return PORT_ROM_NOT_FOUND;
    }
    if (got != (s32) sizeof(header) || !normalise_rom_byte_order(header, sizeof(header))) {
        return PORT_ROM_NOT_N64;
    }
    return (bcmp(header + 0x20, "Diddy Kong Racing", 17) == 0) ? PORT_ROM_OK : PORT_ROM_NOT_DKR;
}

s32 port_rom_load(const char *path) {
    unsigned int size;
    u32 count, lutStart, lutEnd;
    u8 *data;
    s32 headerResult = is_dkr_header(path);

    if (headerResult != PORT_ROM_OK) {
        return headerResult;
    }
    data = plat_load_file(path, &size);
    if (data == NULL) {
        return PORT_ROM_NOT_FOUND;
    }
    if (!normalise_rom_byte_order(data, size)) {
        plat_free(data);
        return PORT_ROM_NOT_N64;
    }
    // The internal name in the header.
    if (bcmp(data + 0x20, "Diddy Kong Racing", 17) != 0) {
        plat_free(data);
        return PORT_ROM_NOT_DKR;
    }
    // The table starts with the number of sections and is followed by (count + 1) offsets.
    lutStart = ASSETS_LUT_ROM_OFFSET;
    if (lutStart + 4 > size) {
        plat_free(data);
        return PORT_ROM_WRONG_VERSION;
    }
    count = port_read_be32(data + lutStart);
    lutEnd = lutStart + (count + 2) * 4;
    if (count < 40 || count > 64 || lutEnd > size || port_read_be32(data + lutEnd - 4) + lutEnd > size) {
        plat_free(data);
        return PORT_ROM_WRONG_VERSION;
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
    if (romOffset + size > sRomSize) {
        bzero((u8 *) dst + (sRomSize - romOffset), romOffset + size - sRomSize);
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
