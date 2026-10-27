/* Backups of the saves: see save_backups.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "plat/plat.h"
#include "plat/save_backups.h"

/* port.h (the game's types clash with the platform's). */
long port_autorun_enabled(void);
int port_save_backup(const char *dir, int onGameThread);
int port_save_restore(const unsigned char *eeprom, const unsigned char *pak, const char *undoDir, int onGameThread);
#define PORT_EEPROM_SIZE 512
#define PORT_MEMPAK_SIZE 0x8000

#define BACKUP_DIR "backup"
#define MAX_BACKUPS 32
#define MAX_FILES 16
#define NAME_SIZE 64

typedef struct Backup {
    char name[48];       // The folder's name.
    char eep[NAME_SIZE]; // Its EEPROM and Controller Pak files, "" if it has none.
    char mpk[NAME_SIZE];
} Backup;

static Backup sBackups[MAX_BACKUPS];
static int sBackupCount;

static int has_extension(const char *name, const char *ext) {
    size_t length = strlen(name), extLength = strlen(ext);
    return length > extLength && strcasecmp(name + length - extLength, ext) == 0;
}

static int newest_first(const void *a, const void *b) {
    return strcmp(((const Backup *) b)->name, ((const Backup *) a)->name);
}

int save_backups_scan(void) {
    static char sFolders[MAX_BACKUPS * 2][48];
    static char sFiles[MAX_FILES][NAME_SIZE];
    char path[192];
    int folders, files, i, j;

    sBackupCount = 0;
    snprintf(path, sizeof(path), "%s/" BACKUP_DIR, plat_data_dir());
    folders = plat_list_dir(path, sFolders[0], sizeof(sFolders[0]), MAX_BACKUPS * 2);
    for (i = 0; i < folders && sBackupCount < MAX_BACKUPS; i++) {
        Backup *backup = &sBackups[sBackupCount];
        memset(backup, 0, sizeof(*backup));
        snprintf(backup->name, sizeof(backup->name), "%s", sFolders[i]);
        snprintf(path, sizeof(path), "%s/" BACKUP_DIR "/%s", plat_data_dir(), sFolders[i]);
        files = plat_list_dir(path, sFiles[0], sizeof(sFiles[0]), MAX_FILES);
        for (j = 0; j < files; j++) {
            // Ours are dkr.eep and dkr.mpk; an emulator's are named after the game.
            if (backup->eep[0] == '\0' && has_extension(sFiles[j], ".eep")) {
                snprintf(backup->eep, sizeof(backup->eep), "%s", sFiles[j]);
            } else if (backup->mpk[0] == '\0' && has_extension(sFiles[j], ".mpk")) {
                snprintf(backup->mpk, sizeof(backup->mpk), "%s", sFiles[j]);
            }
        }
        if (backup->eep[0] != '\0' || backup->mpk[0] != '\0') {
            sBackupCount++;
        }
    }
    qsort(sBackups, sBackupCount, sizeof(Backup), newest_first);
    return sBackupCount;
}

int save_backups_count(void) {
    return sBackupCount;
}

void save_backups_name(int i, char *out, int size) {
    const char *n = i >= 0 && i < sBackupCount ? sBackups[i].name : "";
    if (strlen(n) >= 17 && n[4] == '-' && n[7] == '-' && n[10] == '_') {
        snprintf(out, size, "%.5s %.2s:%.2s%s", n + 5, n + 11, n + 13, save_backups_is_undo(i) ? " undo" : "");
    } else {
        snprintf(out, size, "%.16s", n);
    }
}

int save_backups_is_undo(int i) {
    return i >= 0 && i < sBackupCount && strstr(sBackups[i].name, "-undo") != NULL;
}

/* Makes backup/<date><suffix> and returns its path under the data folder in out. */
static void make_dated_folder(const char *suffix, char *out, int size) {
    char path[192];
    time_t now = time(NULL);
    struct tm *t = gmtime(&now); // The 3DS's clock is local time.
    snprintf(out, size, BACKUP_DIR "/%04d-%02d-%02d_%02d%02d%02d%s", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec, suffix);
    snprintf(path, sizeof(path), "%s/" BACKUP_DIR, plat_data_dir());
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/%s", plat_data_dir(), out);
    mkdir(path, 0777);
}

int save_backups_back_up(int onGameThread) {
    char dir[64];
    int result;
    if (port_autorun_enabled()) {
        return SAVE_BACKUP_TEST_RUN;
    }
    make_dated_folder("", dir, sizeof(dir));
    result = port_save_backup(dir, onGameThread);
    save_backups_scan();
    return result > 0 ? SAVE_BACKUP_DONE : result < 0 ? SAVE_BACKUP_BUSY : SAVE_BACKUP_WRITE_FAILED;
}

/* Reads a backup's file into buffer (size bytes; a longer file, such as a 2 KB .eep, is cut). */
static int read_file(const Backup *backup, const char *file, unsigned char *buffer, unsigned int size) {
    char path[192];
    snprintf(path, sizeof(path), "%s/" BACKUP_DIR "/%s/%s", plat_data_dir(), backup->name, file);
    return plat_load_file_start(path, buffer, size) == (int) size;
}

int save_backups_restore(int i, int onGameThread) {
    static unsigned char sEeprom[PORT_EEPROM_SIZE];
    static unsigned char sPak[PORT_MEMPAK_SIZE];
    const Backup *backup;
    int haveEeprom, havePak, result;
    char undo[64];

    if (port_autorun_enabled()) {
        return SAVE_BACKUP_TEST_RUN;
    }
    if (i < 0 || i >= sBackupCount) {
        return SAVE_BACKUP_TOO_SHORT;
    }
    backup = &sBackups[i];
    haveEeprom = backup->eep[0] != '\0' && read_file(backup, backup->eep, sEeprom, sizeof(sEeprom));
    havePak = backup->mpk[0] != '\0' && read_file(backup, backup->mpk, sPak, sizeof(sPak));
    if (!haveEeprom && !havePak) {
        return SAVE_BACKUP_TOO_SHORT;
    }
    make_dated_folder("-undo", undo, sizeof(undo));
    result = port_save_restore(haveEeprom ? sEeprom : NULL, havePak ? sPak : NULL, undo, onGameThread);
    return result > 0 ? SAVE_BACKUP_DONE : result < 0 ? SAVE_BACKUP_BUSY : SAVE_BACKUP_WRITE_FAILED;
}
