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

/*
 * The saves are written from the game's thread and, when the HOME menu opens, the console sleeps or the game exits,
 * from the main thread, which has a higher priority. This lock keeps the two from writing at once, and keeps the main
 * thread from copying a save the game is part way through changing. It sleeps rather than spins, so that the main
 * thread lets the game's finish.
 */
static volatile s32 sSaveLock;

static void save_lock(void) {
    while (__atomic_exchange_n(&sSaveLock, 1, __ATOMIC_ACQUIRE)) {
        plat_sleep_ns(1000000ULL);
    }
}

static void save_unlock(void) {
    __atomic_store_n(&sSaveLock, 0, __ATOMIC_RELEASE);
}

/* Write the EEPROM and the Controller Pak, in the N64's order, as dir + dkr.eep and dir + dkr.mpk. Called locked. */
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

/* Writes the saves that changed. A write that fails leaves its save marked, so it is tried again later. Locked. */
static void write_dirty_saves(void) {
    static s32 sFailures;
    s32 ok = TRUE;

    if (sEepromDirty) {
        sEepromDirty = FALSE;
        if (!write_eeprom("")) {
            sEepromDirty = TRUE;
            ok = FALSE;
        }
    }
    if (sMempakDirty) {
        sMempakDirty = FALSE;
        if (!write_mempak("")) {
            sMempakDirty = TRUE;
            ok = FALSE;
        }
    }
    if (!ok) {
        // Not again for the flush delay, so a full or locked SD card isn't tried every frame.
        sLastSaveWrite = plat_time_ns();
        if (sFailures++ < 5) {
            plat_log("saves: writing to the SD card failed, will try again");
        }
    } else {
        sFailures = 0;
    }
}

static void flush_saves(s32 force) {
    if (!force) {
        do_save_request();
    }
    if (!sEepromDirty && !sMempakDirty) {
        return;
    }
    save_lock();
    if (port_autorun_enabled() || sSavesFrozen) {
        // The unattended test run never touches the player's saves (its races would set track records), and after a
        // restore the game's memory has the old progress.
        sEepromDirty = FALSE;
        sMempakDirty = FALSE;
    } else if (force || plat_time_ns() - sLastSaveWrite >= SAVE_FLUSH_DELAY_NS) {
        write_dirty_saves();
    }
    save_unlock();
}

/******************************** Backups ********************************/

enum { REQUEST_NONE, REQUEST_ASKED, REQUEST_TAKEN, REQUEST_DONE };
enum { REQUEST_BACKUP, REQUEST_RESTORE };

typedef struct SaveRequest {
    s32 kind;
    const char *dir; // Backup: where to; restore: where the saves go first (NULL: nowhere).
    const u8 *eeprom;
    const u8 *pak;
} SaveRequest;

/* The request another thread hands to the game's. Only one at a time: sAskLock is held from asking to the answer. */
static volatile s32 sRequestState;
static volatile s32 sAskLock;
static SaveRequest sRequest;
static s32 sRequestResult;

/* On the game's thread: a backup or restore. */
static s32 run_request(const SaveRequest *request) {
    char dir[96];
    s32 ok = TRUE;

    save_lock();
    if (request->dir != NULL) {
        plat_snprintf(dir, sizeof(dir), "%s/", request->dir);
        ok = write_eeprom(dir) && write_mempak(dir);
    }
    if (ok && request->kind == REQUEST_RESTORE && sSavesFrozen) {
        plat_log("saves: already restored, the game has to restart first");
        ok = FALSE;
    } else if (ok && request->kind == REQUEST_RESTORE) {
        // A save the game made in the last moments is written first, so the one of the pair not restored has it.
        write_dirty_saves();
        ok = !sEepromDirty && !sMempakDirty;
        if (ok && request->eeprom != NULL) {
            ok = port_save_write(EEPROM_FILE, request->eeprom, EEPROM_SIZE);
        }
        if (ok && request->pak != NULL) {
            ok = port_save_write(MEMPAK_FILE, request->pak, MEMPAK_SIZE);
        }
        if (ok) {
            sSavesFrozen = TRUE;
        } else {
            // Put back what the game holds (just written above), so a half restore doesn't stay, and keep saving.
            sEepromDirty = TRUE;
            sMempakDirty = TRUE;
            write_dirty_saves();
        }
        plat_log("saves: restore%s%s%s", request->eeprom != NULL ? " " EEPROM_FILE : "",
                 request->pak != NULL ? " " MEMPAK_FILE : "", ok ? " done" : " failed, saves kept as they were");
    } else {
        plat_log("saves: backup to %s %s", request->dir, ok ? "written" : "failed");
    }
    save_unlock();
    return ok;
}

/* On the game's thread, at a controller read: a request another thread made. */
static void do_save_request(void) {
    s32 expected = REQUEST_ASKED;

    if (!__atomic_compare_exchange_n(&sRequestState, &expected, REQUEST_TAKEN, FALSE, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED)) {
        return;
    }
    sRequestResult = run_request(&sRequest);
    __atomic_store_n(&sRequestState, REQUEST_DONE, __ATOMIC_RELEASE);
}

/* From another thread: hands the request to the game's thread and waits for it. On the game's own: does it now. */
static s32 run_save_request(const SaveRequest *request, s32 onGameThread) {
    unsigned long long start = plat_time_ns();
    s32 expected, result;

    if (onGameThread) {
        return run_request(request);
    }
    while (__atomic_exchange_n(&sAskLock, 1, __ATOMIC_ACQUIRE)) {
        plat_sleep_ns(20000000ULL);
    }
    sRequest = *request;
    __atomic_store_n(&sRequestState, REQUEST_ASKED, __ATOMIC_RELEASE);
    for (;;) {
        plat_sleep_ns(20000000ULL);
        expected = REQUEST_ASKED;
        if (plat_time_ns() - start > 3000000000ULL &&
            __atomic_compare_exchange_n(&sRequestState, &expected, REQUEST_NONE, FALSE, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            // The game isn't reading the controllers (loading, or stopped): nothing was done.
            result = -1;
            break;
        }
        if (__atomic_load_n(&sRequestState, __ATOMIC_ACQUIRE) == REQUEST_DONE) {
            sRequestState = REQUEST_NONE;
            result = sRequestResult;
            break;
        }
    }
    __atomic_store_n(&sAskLock, 0, __ATOMIC_RELEASE);
    return result;
}

long port_save_backup(const char *dir, long onGameThread) {
    SaveRequest request = { REQUEST_BACKUP, dir, NULL, NULL };
    return run_save_request(&request, onGameThread);
}

long port_save_restore(const unsigned char *eeprom, const unsigned char *pak, const char *undoDir,
                       long onGameThread) {
    SaveRequest request = { REQUEST_RESTORE, undoDir, eeprom, pak };
    return run_save_request(&request, onGameThread);
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
    if (port_autorun_enabled()) {
        // The unattended test run drives everything itself (the karts by the AI, the menus by direct calls): a stray
        // press, such as TRY AGAIN after a race, would break it. The bottom screen's Stop button is the way out.
        for (s32 i = 0; i < MAXCONTROLLERS; i++) {
            sControllers[i].buttons = 0;
            sControllers[i].stickX = 0;
            sControllers[i].stickY = 0;
        }
    }
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
    } else if (!port_save_exists(EEPROM_FILE) && port_save_read(OLD_EEPROM_FILE, sEeprom, sizeof(sEeprom))) {
        // An earlier build's save: kept as it was, written again in the N64's order. Only when there is no
        // dkr.eep at all: an empty one (cut off by an earlier build's save) must not bring back that old progress.
        plat_log("saves: " OLD_EEPROM_FILE " -> " EEPROM_FILE);
        sEepromDirty = TRUE;
    } else {
        bzero(sEeprom, sizeof(sEeprom));
    }
    if (port_save_read(MEMPAK_FILE, sMempak, sizeof(sMempak))) {
        port_mpk_convert(sMempak, FALSE);
    } else if (!port_save_exists(MEMPAK_FILE) && port_save_read(OLD_MEMPAK_FILE, sMempak, sizeof(sMempak))) {
        plat_log("saves: " OLD_MEMPAK_FILE " -> " MEMPAK_FILE);
        sMempakDirty = TRUE;
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

static s32 eeprom_write_block(u8 address, u8 *buffer) {
    if (address >= EEPROM_SIZE / EEPROM_BLOCK_SIZE) {
        return -1;
    }
    bcopy(buffer, &sEeprom[address * EEPROM_BLOCK_SIZE], EEPROM_BLOCK_SIZE);
    sEepromDirty = TRUE;
    sLastSaveWrite = plat_time_ns();
    return 0;
}

s32 osEepromWrite(OSMesgQueue *mq, u8 address, u8 *buffer) {
    s32 ret;
    (void) mq;
    save_lock();
    ret = eeprom_write_block(address, buffer);
    save_unlock();
    return ret;
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

/* Locked as a whole, so a save written from the main thread never has half of it. */
s32 osEepromLongWrite(OSMesgQueue *mq, u8 address, u8 *buffer, int nbytes) {
    s32 ret = 0;
    (void) mq;
    save_lock();
    while (nbytes > 0 && ret == 0) {
        ret = eeprom_write_block(address++, buffer);
        buffer += EEPROM_BLOCK_SIZE;
        nbytes -= EEPROM_BLOCK_SIZE;
    }
    save_unlock();
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
        save_lock();
        bcopy(buffer, &sMempak[address * BLOCKSIZE], BLOCKSIZE);
        sMempakDirty = TRUE;
        sLastSaveWrite = plat_time_ns();
        save_unlock();
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
