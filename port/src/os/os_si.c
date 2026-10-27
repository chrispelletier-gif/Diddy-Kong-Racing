/*
 * Serial interface emulation: controllers, the cartridge EEPROM and a Controller Pak in port 1.
 *
 * The Controller Pak filesystem itself is libultra's own code (libultra/src/io/pfs*.c); only the raw 32 byte
 * block reads and writes it is built on are emulated here, backed by a 32KB file.
 *
 * The files, dkr.eep and dkr.mpk in the data folder, are in the N64's byte order, so they are interchangeable with an
 * emulator's or a real cartridge's (save_convert.c); in memory they are in the order the game's code reads. Builds
 * before that saved eeprom.bin and controllerpak.bin in memory order; those are read once and left as they were.
 */
#include <ultra64.h>
#include "PR/os_internal.h"
#include "PRinternal/controller.h"
#include "types.h"
#include "port.h"
#include "plat/plat.h"

#define EEPROM_SIZE PORT_EEPROM_SIZE /* 4Kbit */
#define MEMPAK_SIZE PORT_MEMPAK_SIZE
#define MEMPAK_BLOCKS (MEMPAK_SIZE / BLOCKSIZE)
/* How long to wait after the last write before saving to storage, in nanoseconds. */
#define SAVE_FLUSH_DELAY_NS 500000000ULL

static u8 sEeprom[EEPROM_SIZE];
static u8 sMempak[MEMPAK_SIZE];
static s32 sEepromDirty;
static s32 sMempakDirty;
static unsigned long long sLastSaveWrite;
static s32 sSaveLoaded;
/* After a restore: the game's memory still has the old progress, which must not be written over the restored one. */
static s32 sSavesFrozen;
static PortControllerState sControllers[MAXCONTROLLERS];

u8 __osContLastCmd;
u8 __osMaxControllers = MAXCONTROLLERS;

#define EEPROM_FILE "dkr.eep"
#define MEMPAK_FILE "dkr.mpk"
/* Earlier builds' files, in memory order. */
#define OLD_EEPROM_FILE "eeprom.bin"
#define OLD_MEMPAK_FILE "controllerpak.bin"

static void load_saves(void);
static void do_save_request(void);

/* Write the EEPROM and the Controller Pak, in the N64's order, as dir + dkr.eep and dir + dkr.mpk. */
static s32 write_eeprom(const char *dir) {
    static u8 sOut[EEPROM_SIZE];
    char name[96];
    bcopy(sEeprom, sOut, EEPROM_SIZE);
    port_eep_convert(sOut);
    plat_snprintf(name, sizeof(name), "%s%s", dir, EEPROM_FILE);
    return port_save_write(name, sOut, EEPROM_SIZE);
}

static s32 write_mempak(const char *dir) {
    static u8 sOut[MEMPAK_SIZE];
    char name[96];
    bcopy(sMempak, sOut, MEMPAK_SIZE);
    port_mpk_convert(sOut, TRUE);
    plat_snprintf(name, sizeof(name), "%s%s", dir, MEMPAK_FILE);
    return port_save_write(name, sOut, MEMPAK_SIZE);
}

static void flush_saves(s32 force) {
    if (!force) {
        do_save_request();
    }
    if (!sEepromDirty && !sMempakDirty) {
        return;
    }
    if (port_autorun_enabled() || sSavesFrozen) {
        // The unattended test run never touches the player's saves (its races would set track records), and after a
        // restore the game's memory has the old progress.
        sEepromDirty = FALSE;
        sMempakDirty = FALSE;
        return;
    }
    if (!force && plat_time_ns() - sLastSaveWrite < SAVE_FLUSH_DELAY_NS) {
        return;
    }
    if (sEepromDirty) {
        sEepromDirty = FALSE;
        write_eeprom("");
    }
    if (sMempakDirty) {
        sMempakDirty = FALSE;
        write_mempak("");
    }
}

/******************************** Backups ********************************/

enum { REQUEST_NONE, REQUEST_ASKED, REQUEST_TAKEN, REQUEST_DONE };
enum { REQUEST_BACKUP, REQUEST_RESTORE };

static volatile s32 sRequestState;
static s32 sRequestKind;
static const char *sRequestDir; // Backup: where to; restore: where the saves go first (NULL: nowhere).
static const u8 *sRestoreEeprom;
static const u8 *sRestorePak;
static s32 sRequestResult;

/* On the game's thread: a backup or restore another thread asked for. */
static void do_save_request(void) {
    char dir[96];
    s32 ok = TRUE;
    s32 expected = REQUEST_ASKED;

    if (!__atomic_compare_exchange_n(&sRequestState, &expected, REQUEST_TAKEN, FALSE, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED)) {
        return;
    }
    if (sRequestDir != NULL) {
        plat_snprintf(dir, sizeof(dir), "%s/", sRequestDir);
        ok = write_eeprom(dir) && write_mempak(dir);
    }
    if (ok && sRequestKind == REQUEST_RESTORE) {
        // A save the game made in the last moments is written first, so the one of the pair not restored has it.
        flush_saves(TRUE);
        sSavesFrozen = TRUE;
        if (sRestoreEeprom != NULL) {
            ok = port_save_write(EEPROM_FILE, sRestoreEeprom, EEPROM_SIZE);
        }
        if (sRestorePak != NULL) {
            ok = port_save_write(MEMPAK_FILE, sRestorePak, MEMPAK_SIZE) && ok;
        }
        plat_log("saves: restored%s%s%s", sRestoreEeprom != NULL ? " " EEPROM_FILE : "",
                 sRestorePak != NULL ? " " MEMPAK_FILE : "", ok ? "" : " (a write failed)");
    } else {
        plat_log("saves: backup to %s %s", sRequestDir, ok ? "written" : "failed");
    }
    sRequestResult = ok;
    __atomic_store_n(&sRequestState, REQUEST_DONE, __ATOMIC_RELEASE);
}

/* From another thread: hands the request to the game's thread and waits for it. On the game's own: does it now. */
static s32 run_save_request(s32 kind, s32 onGameThread) {
    unsigned long long start = plat_time_ns();
    s32 expected;

    sRequestKind = kind;
    __atomic_store_n(&sRequestState, REQUEST_ASKED, __ATOMIC_RELEASE);
    if (onGameThread) {
        do_save_request();
        sRequestState = REQUEST_NONE;
        return sRequestResult;
    }
    for (;;) {
        plat_sleep_ns(20000000ULL);
        expected = REQUEST_ASKED;
        if (plat_time_ns() - start > 3000000000ULL &&
            __atomic_compare_exchange_n(&sRequestState, &expected, REQUEST_NONE, FALSE, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            // The game isn't reading the controllers (loading, or stopped): nothing was done.
            return -1;
        }
        if (__atomic_load_n(&sRequestState, __ATOMIC_ACQUIRE) == REQUEST_DONE) {
            sRequestState = REQUEST_NONE;
            return sRequestResult;
        }
    }
}

s32 port_save_backup(const char *dir, s32 onGameThread) {
    sRequestDir = dir;
    return run_save_request(REQUEST_BACKUP, onGameThread);
}

s32 port_save_restore(const u8 *eeprom, const u8 *pak, const char *undoDir, s32 onGameThread) {
    sRequestDir = undoDir;
    sRestoreEeprom = eeprom;
    sRestorePak = pak;
    return run_save_request(REQUEST_RESTORE, onGameThread);
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
    // The look-back button isn't an N64 button: the camera reads it (port_camera.c), the game never sees it.
    port_set_look_back_held((sControllers[0].buttons & PORT_BUTTON_LOOK_BACK) != 0);
    for (i = 0; i < MAXCONTROLLERS; i++) {
        data[i].button = sControllers[i].buttons & ~PORT_BUTTON_LOOK_BACK;
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
    if (port_save_read(EEPROM_FILE, sEeprom, sizeof(sEeprom))) {
        port_eep_convert(sEeprom);
    } else if (port_save_read(OLD_EEPROM_FILE, sEeprom, sizeof(sEeprom))) {
        // An earlier build's save: kept as it was, written again in the N64's order.
        plat_log("saves: " OLD_EEPROM_FILE " -> " EEPROM_FILE);
        write_eeprom("");
    } else {
        bzero(sEeprom, sizeof(sEeprom));
    }
    if (port_save_read(MEMPAK_FILE, sMempak, sizeof(sMempak))) {
        port_mpk_convert(sMempak, FALSE);
    } else if (port_save_read(OLD_MEMPAK_FILE, sMempak, sizeof(sMempak))) {
        plat_log("saves: " OLD_MEMPAK_FILE " -> " MEMPAK_FILE);
        write_mempak("");
    } else {
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
