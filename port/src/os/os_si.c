/*
 * Serial interface emulation: controllers, the cartridge EEPROM and a Controller Pak in port 1.
 *
 * The Controller Pak filesystem itself is libultra's own code (libultra/src/io/pfs*.c); only the raw 32 byte
 * block reads and writes it is built on are emulated here, backed by a 32KB file.
 */
#include <ultra64.h>
#include "PR/os_internal.h"
#include "PRinternal/controller.h"
#include "types.h"
#include "port.h"
#include "plat/plat.h"

#define EEPROM_SIZE 512 /* 4Kbit */
#define MEMPAK_SIZE 0x8000
#define MEMPAK_BLOCKS (MEMPAK_SIZE / BLOCKSIZE)
/* How long to wait after the last write before saving to storage, in nanoseconds. */
#define SAVE_FLUSH_DELAY_NS 500000000ULL

static u8 sEeprom[EEPROM_SIZE];
static u8 sMempak[MEMPAK_SIZE];
static s32 sEepromDirty;
static s32 sMempakDirty;
static unsigned long long sLastSaveWrite;
static s32 sSaveLoaded;
static PortControllerState sControllers[MAXCONTROLLERS];

u8 __osContLastCmd;
u8 __osMaxControllers = MAXCONTROLLERS;

#define EEPROM_FILE "eeprom.bin"
#define MEMPAK_FILE "controllerpak.bin"

static void load_saves(void);

static void flush_saves(s32 force) {
    if (!sEepromDirty && !sMempakDirty) {
        return;
    }
    if (port_autorun_enabled()) {
        // The unattended test run never touches the player's saves (its races would set track records).
        sEepromDirty = FALSE;
        sMempakDirty = FALSE;
        return;
    }
    if (!force && plat_time_ns() - sLastSaveWrite < SAVE_FLUSH_DELAY_NS) {
        return;
    }
    if (sEepromDirty) {
        sEepromDirty = FALSE;
        port_save_write(EEPROM_FILE, sEeprom, sizeof(sEeprom));
    }
    if (sMempakDirty) {
        sMempakDirty = FALSE;
        port_save_write(MEMPAK_FILE, sMempak, sizeof(sMempak));
    }
}

void port_si_flush(void) {
    flush_saves(TRUE);
}

void port_si_init(void) {
    load_saves();
}

/******************************** Controllers ********************************/

s32 osContInit(OSMesgQueue *mq, u8 *bitpattern, OSContStatus *data) {
    s32 i;
    u8 pattern = 0;

    (void) mq;
    load_saves();
    port_input_poll(sControllers, MAXCONTROLLERS);
    for (i = 0; i < MAXCONTROLLERS; i++) {
        data[i].type = CONT_TYPE_NORMAL;
        data[i].status = (i == 0) ? CONT_CARD_ON : 0;
        data[i].errno = sControllers[i].connected ? 0 : CONT_NO_RESPONSE_ERROR;
        if (sControllers[i].connected) {
            pattern |= 1 << i;
        }
    }
    *bitpattern = pattern;
    return 0;
}

s32 osContStartQuery(OSMesgQueue *mq) {
    port_post_mesg(mq, NULL);
    return 0;
}

void osContGetQuery(OSContStatus *data) {
    s32 i;
    for (i = 0; i < MAXCONTROLLERS; i++) {
        data[i].type = CONT_TYPE_NORMAL;
        data[i].status = (i == 0) ? CONT_CARD_ON : 0;
        data[i].errno = sControllers[i].connected ? 0 : CONT_NO_RESPONSE_ERROR;
    }
}

s32 osContStartReadData(OSMesgQueue *mq) {
    port_input_poll(sControllers, MAXCONTROLLERS);
    flush_saves(FALSE);
    port_post_mesg(mq, NULL);
    return 0;
}

void osContGetReadData(OSContPad *data) {
    s32 i;
    for (i = 0; i < MAXCONTROLLERS; i++) {
        data[i].button = sControllers[i].buttons;
        data[i].stick_x = sControllers[i].stickX;
        data[i].stick_y = sControllers[i].stickY;
        data[i].errno = sControllers[i].connected ? 0 : CONT_NO_RESPONSE_ERROR;
    }
}

/******************************** EEPROM ********************************/

static void load_saves(void) {
    if (sSaveLoaded) {
        return;
    }
    sSaveLoaded = TRUE;
    if (!port_save_read(EEPROM_FILE, sEeprom, sizeof(sEeprom))) {
        bzero(sEeprom, sizeof(sEeprom));
    }
    if (!port_save_read(MEMPAK_FILE, sMempak, sizeof(sMempak))) {
        // A brand new pak. It gets formatted the first time the game initialises it.
        bzero(sMempak, sizeof(sMempak));
    }
}

s32 osEepromProbe(OSMesgQueue *mq) {
    (void) mq;
    load_saves();
    return EEPROM_TYPE_4K;
}

s32 osEepromRead(OSMesgQueue *mq, u8 address, u8 *buffer) {
    (void) mq;
    if (address >= EEPROM_SIZE / EEPROM_BLOCK_SIZE) {
        return -1;
    }
    bcopy(&sEeprom[address * EEPROM_BLOCK_SIZE], buffer, EEPROM_BLOCK_SIZE);
    return 0;
}

s32 osEepromWrite(OSMesgQueue *mq, u8 address, u8 *buffer) {
    (void) mq;
    if (address >= EEPROM_SIZE / EEPROM_BLOCK_SIZE) {
        return -1;
    }
    bcopy(buffer, &sEeprom[address * EEPROM_BLOCK_SIZE], EEPROM_BLOCK_SIZE);
    sEepromDirty = TRUE;
    sLastSaveWrite = plat_time_ns();
    return 0;
}

s32 osEepromLongRead(OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes) {
    s32 ret = 0;
    while (nbytes > 0 && ret == 0) {
        ret = osEepromRead(mq, address++, buffer);
        buffer += EEPROM_BLOCK_SIZE;
        nbytes -= EEPROM_BLOCK_SIZE;
    }
    return ret;
}

s32 osEepromLongWrite(OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes) {
    s32 ret = 0;
    while (nbytes > 0 && ret == 0) {
        ret = osEepromWrite(mq, address++, buffer);
        buffer += EEPROM_BLOCK_SIZE;
        nbytes -= EEPROM_BLOCK_SIZE;
    }
    return ret;
}

/******************************** Controller Pak ********************************/

void __osSiGetAccess(void) {
}

void __osSiRelAccess(void) {
}

s32 __osContRamRead(OSMesgQueue *mq, int channel, u16 address, u8 *buffer) {
    (void) mq;
    if (channel != 0) {
        return PFS_ERR_NOPACK;
    }
    if (address < MEMPAK_BLOCKS) {
        bcopy(&sMempak[address * BLOCKSIZE], buffer, BLOCKSIZE);
    } else {
        // Accessory registers (bank select, rumble detection). A plain Controller Pak reads back zeros.
        bzero(buffer, BLOCKSIZE);
    }
    return 0;
}

s32 __osContRamWrite(OSMesgQueue *mq, int channel, u16 address, u8 *buffer, int force) {
    (void) mq;
    (void) force;
    if (channel != 0) {
        return PFS_ERR_NOPACK;
    }
    if (address < MEMPAK_BLOCKS) {
        bcopy(buffer, &sMempak[address * BLOCKSIZE], BLOCKSIZE);
        sMempakDirty = TRUE;
        sLastSaveWrite = plat_time_ns();
    }
    return 0;
}

s32 __osPfsGetStatus(OSMesgQueue *queue, int channel) {
    (void) queue;
    load_saves();
    return (channel == 0) ? 0 : PFS_ERR_NOPACK;
}

s32 osPfsInit(OSMesgQueue *queue, OSPfs *pfs, int channel) {
    s32 ret;

    ret = __osPfsGetStatus(queue, channel);
    if (ret != 0) {
        return ret;
    }
    pfs->queue = queue;
    pfs->channel = channel;
    pfs->status = 0;
    ret = __osGetId(pfs);
    if (ret == PFS_ERR_ID_FATAL || ret == PFS_ERR_DEVICE) {
        // An unformatted pak (first run). Format it so it behaves like a new, empty Controller Pak.
        ret = osPfsReFormat(pfs, queue, channel);
        if (ret == 0) {
            pfs->status = 0;
            ret = __osGetId(pfs);
        }
    }
    if (ret != 0) {
        return ret;
    }
    ret = osPfsChecker(pfs);
    pfs->status |= PFS_INITIALIZED;
    return ret;
}

s32 osPfsIsPlug(OSMesgQueue *mq, u8 *pattern) {
    (void) mq;
    *pattern = 1; // Only controller 1 has a pak.
    return 0;
}

/******************************** Rumble Pak ********************************/

s32 osMotorInit(OSMesgQueue *mq, OSPfs *pfs, int channel) {
    (void) mq;
    pfs->channel = channel;
    pfs->status = 0;
    // Port 1 holds the Controller Pak, the others are empty.
    return (channel == 0) ? PFS_ERR_DEVICE : PFS_ERR_NOPACK;
}

#ifdef osMotorStart
#undef osMotorStart
#undef osMotorStop
#endif

#ifndef MOTOR_START
#define MOTOR_START 1
#define MOTOR_STOP 0
#endif

s32 __osMotorAccess(OSPfs *pfs, s32 flag) {
    port_rumble(pfs->channel, flag == MOTOR_START);
    return 0;
}

s32 osMotorStart(OSPfs *pfs) {
    return __osMotorAccess(pfs, MOTOR_START);
}

s32 osMotorStop(OSPfs *pfs) {
    return __osMotorAccess(pfs, MOTOR_STOP);
}
