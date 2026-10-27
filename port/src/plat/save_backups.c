/* Backups of the saves: see save_backups.h. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "plat/plat.h"
#include "plat/save_backups.h"
#include "port_saves.h"

#define BACKUP_DIR "backup"
#define MAX_FOLDERS 256
#define MAX_FILES 16
#define NAME_SIZE 64

/* The scan's name buffers are shared, and the Settings menu and SAVE OPTIONS can scan at the same time. */
static volatile int sScanLock;

static void scan_lock(void) {
    while (__atomic_exchange_n(&sScanLock, 1, __ATOMIC_ACQUIRE)) {
        plat_sleep_ns(1000000ULL); // A sleep, not a spin: the holder may be a lower priority thread on this core.
    }
}

static void scan_unlock(void) {
    __atomic_store_n(&sScanLock, 0, __ATOMIC_RELEASE);
}

static int has_extension(const char *name, const char *ext) {
    size_t length = strlen(name), extLength = strlen(ext);
    return length > extLength && strcasecmp(name + length - extLength, ext) == 0;
}

static int newest_first(const void *a, const void *b) {
    return strcmp((const char *) b, (const char *) a);
}

int save_backups_scan(SaveBackupList *list) {
    static char sFolders[MAX_FOLDERS][48];
    static char sFiles[MAX_FILES][NAME_SIZE];
    char path[192];
    int folders, files, i, j;

    scan_lock();
    list->count = 0;
    snprintf(path, sizeof(path), "%s/" BACKUP_DIR, plat_data_dir());
    // The whole arrays, not sFolders[0]: the fortified strcpy would take that for a single 48 byte name.
    folders = plat_list_dir(path, (char *) sFolders, sizeof(sFolders[0]), MAX_FOLDERS);
    // Sorted before the newest are picked: the SD card lists folders oldest first.
    qsort(sFolders, folders, sizeof(sFolders[0]), newest_first);
    for (i = 0; i < folders && list->count < SAVE_BACKUPS_MAX; i++) {
        SaveBackup *backup = &list->backups[list->count];
        memset(backup, 0, sizeof(*backup));
        snprintf(backup->name, sizeof(backup->name), "%s", sFolders[i]);
        snprintf(path, sizeof(path), "%s/" BACKUP_DIR "/%s", plat_data_dir(), sFolders[i]);
        files = plat_list_dir(path, (char *) sFiles, sizeof(sFiles[0]), MAX_FILES);
        for (j = 0; j < files; j++) {
            // Ours are dkr.eep and dkr.mpk; an emulator's are named after the game.
            if (backup->eep[0] == '\0' && has_extension(sFiles[j], ".eep")) {
                snprintf(backup->eep, sizeof(backup->eep), "%s", sFiles[j]);
            } else if (backup->mpk[0] == '\0' && has_extension(sFiles[j], ".mpk")) {
                snprintf(backup->mpk, sizeof(backup->mpk), "%s", sFiles[j]);
            }
        }
        if (backup->eep[0] != '\0' || backup->mpk[0] != '\0') {
            list->count++;
        }
    }
    scan_unlock();
    return list->count;
}

int save_backups_count(const SaveBackupList *list) {
    return list->count;
}

void save_backups_name(const SaveBackupList *list, int i, char *out, int size) {
    const char *n = i >= 0 && i < list->count ? list->backups[i].name : "";
    if (strlen(n) >= 17 && n[4] == '-' && n[7] == '-' && n[10] == '_') {
        snprintf(out, size, "%.5s %.2s:%.2s%s", n + 5, n + 11, n + 13, save_backups_is_undo(list, i) ? " undo" : "");
    } else {
        snprintf(out, size, "%.16s", n);
    }
}

int save_backups_is_undo(const SaveBackupList *list, int i) {
    return i >= 0 && i < list->count && strstr(list->backups[i].name, "-undo") != NULL;
}

/*
 * Makes a new backup/<date><suffix> folder and returns its path under the data folder in out, or 0 if it can't. Two
 * backups in the same second get "-2", "-3"... rather than sharing a folder.
 */
static int make_dated_folder(const char *suffix, char *out, int size) {
    char path[192], date[32];
    time_t now = time(NULL);
    // The 3DS's clock is set to local time; elsewhere time() is UTC.
#ifdef __3DS__
    struct tm *t = gmtime(&now);
#else
    struct tm *t = localtime(&now);
#endif
    int n;

    snprintf(date, sizeof(date), "%04d-%02d-%02d_%02d%02d%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
    snprintf(path, sizeof(path), "%s/" BACKUP_DIR, plat_data_dir());
    mkdir(path, 0777);
    for (n = 1; n < 10; n++) {
        if (n == 1) {
            snprintf(out, size, BACKUP_DIR "/%s%s", date, suffix);
        } else {
            snprintf(out, size, BACKUP_DIR "/%s-%d%s", date, n, suffix);
        }
        snprintf(path, sizeof(path), "%s/%s", plat_data_dir(), out);
        if (mkdir(path, 0777) == 0) {
            return 1;
        }
        if (errno != EEXIST) {
            plat_log("saves: could not make %s (errno %d)", path, errno);
            return 0;
        }
    }
    return 0;
}

int save_backups_back_up(int onGameThread) {
    char dir[64];
    long result;
    if (port_autorun_enabled()) {
        return SAVE_BACKUP_TEST_RUN;
    }
    if (!make_dated_folder("", dir, sizeof(dir))) {
        return SAVE_BACKUP_WRITE_FAILED;
    }
    result = port_save_backup(dir, onGameThread);
    return result > 0 ? SAVE_BACKUP_DONE : result < 0 ? SAVE_BACKUP_BUSY : SAVE_BACKUP_WRITE_FAILED;
}

/*
 * Reads a backup's file into buffer (size bytes; a longer file, such as a 2 KB .eep, is cut). Returns 1 if it read the
 * whole size, 0 if the file is too short, -1 if it can't be opened.
 */
static int read_file(const SaveBackup *backup, const char *file, unsigned char *buffer, unsigned int size) {
    char path[192];
    int got;
    snprintf(path, sizeof(path), "%s/" BACKUP_DIR "/%s/%s", plat_data_dir(), backup->name, file);
    got = plat_load_file_start(path, buffer, size);
    return got < 0 ? -1 : got == (int) size;
}

int save_backups_restore(const SaveBackupList *list, int i, int onGameThread) {
    static unsigned char sEeprom[PORT_EEPROM_SIZE];
    static unsigned char sPak[PORT_MEMPAK_SIZE];
    const SaveBackup *backup;
    int haveEeprom = 0, havePak = 0;
    long result;
    char undo[64];

    if (port_autorun_enabled()) {
        return SAVE_BACKUP_TEST_RUN;
    }
    if (i < 0 || i >= list->count) {
        return SAVE_BACKUP_NOT_FOUND;
    }
    backup = &list->backups[i];
    // Each file the backup has must read in full: half a restore would look like a whole one.
    if (backup->eep[0] != '\0') {
        haveEeprom = read_file(backup, backup->eep, sEeprom, sizeof(sEeprom));
    }
    if (backup->mpk[0] != '\0') {
        havePak = read_file(backup, backup->mpk, sPak, sizeof(sPak));
    }
    if (haveEeprom < 0 || havePak < 0) {
        return SAVE_BACKUP_NOT_FOUND;
    }
    if ((backup->eep[0] != '\0' && !haveEeprom) || (backup->mpk[0] != '\0' && !havePak)) {
        return SAVE_BACKUP_TOO_SHORT;
    }
    if (!make_dated_folder("-undo", undo, sizeof(undo))) {
        return SAVE_BACKUP_WRITE_FAILED;
    }
    result = port_save_restore(haveEeprom ? sEeprom : NULL, havePak ? sPak : NULL, undo, onGameThread);
    return result > 0 ? SAVE_BACKUP_DONE : result < 0 ? SAVE_BACKUP_BUSY : SAVE_BACKUP_WRITE_FAILED;
}
