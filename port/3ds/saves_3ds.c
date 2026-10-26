/*
 * The Settings menu's Saves page: back up the saves to a dated folder and restore one (roadmap O10, Q13).
 *
 * The saves are dkr.eep and dkr.mpk in sdmc:/3ds/dkr/, in the N64's byte order (os/save_convert.c), so they are the
 * same files an N64 emulator keeps. A backup is a folder in sdmc:/3ds/dkr/backup/ named by its date; any other
 * folder there holding an .eep and/or an .mpk file (an emulator's save, under any name) can be restored too, which
 * is how saves come over from an emulator. A restore first backs up the saves it replaces (a folder ending "-undo"),
 * then closes the game: the game keeps its progress in memory, so it has to start again to read the restored one.
 */
#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "plat/plat.h"

void plat_3ds_request_quit(void);
long port_autorun_enabled(void);
/* port.h (the game's types clash with libctru's). */
int port_save_backup(const char *dir);
int port_save_restore(const u8 *eeprom, const u8 *pak, const char *undoDir);
#define PORT_EEPROM_SIZE 512
#define PORT_MEMPAK_SIZE 0x8000

#define BACKUP_DIR "backup"
#define MAX_BACKUPS 32
#define CONFIRM_MS 5000

enum { ROW_BACK_UP, ROW_PICK, ROW_RESTORE, ROW_COUNT };

typedef struct Backup {
    char name[48]; // The folder's name.
    char eep[64];  // Its EEPROM and Controller Pak files, "" if it has none.
    char mpk[64];
} Backup;

static Backup sBackups[MAX_BACKUPS];
static int sBackupCount;
static int sPicked;               // Of sBackups, newest first.
static u64 sArmedAt;              // When Restore was tapped once (0: not), so a second tap restores.
static char sStatus[64];          // What the last action did, shown as the hint of its row.
static int sStatusRow = -1;

static void set_status(int row, const char *text) {
    snprintf(sStatus, sizeof(sStatus), "%s", text);
    sStatusRow = row;
}

static int has_extension(const char *name, const char *ext) {
    size_t length = strlen(name), extLength = strlen(ext);
    return length > extLength && strcasecmp(name + length - extLength, ext) == 0;
}

static int newest_first(const void *a, const void *b) {
    return strcmp(((const Backup *) b)->name, ((const Backup *) a)->name);
}

/* Lists the folders in backup/ that hold a save. */
static void scan(void) {
    char path[160];
    DIR *dir, *sub;
    struct dirent *entry, *file;

    sBackupCount = 0;
    snprintf(path, sizeof(path), "%s/" BACKUP_DIR, plat_data_dir());
    dir = opendir(path);
    if (dir == NULL) {
        return;
    }
    while ((entry = readdir(dir)) != NULL && sBackupCount < MAX_BACKUPS) {
        Backup *backup = &sBackups[sBackupCount];
        if (entry->d_name[0] == '.' || strlen(entry->d_name) >= sizeof(backup->name)) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/" BACKUP_DIR "/%s", plat_data_dir(), entry->d_name);
        sub = opendir(path);
        if (sub == NULL) {
            continue;
        }
        memset(backup, 0, sizeof(*backup));
        snprintf(backup->name, sizeof(backup->name), "%s", entry->d_name);
        while ((file = readdir(sub)) != NULL) {
            if (strlen(file->d_name) >= sizeof(backup->eep)) {
                continue;
            }
            // Ours are dkr.eep and dkr.mpk; an emulator's are named after the game.
            if (backup->eep[0] == '\0' && has_extension(file->d_name, ".eep")) {
                snprintf(backup->eep, sizeof(backup->eep), "%s", file->d_name);
            } else if (backup->mpk[0] == '\0' && has_extension(file->d_name, ".mpk")) {
                snprintf(backup->mpk, sizeof(backup->mpk), "%s", file->d_name);
            }
        }
        closedir(sub);
        if (backup->eep[0] != '\0' || backup->mpk[0] != '\0') {
            sBackupCount++;
        }
    }
    closedir(dir);
    qsort(sBackups, sBackupCount, sizeof(Backup), newest_first);
    if (sPicked >= sBackupCount) {
        sPicked = 0;
    }
}

/* "2026-10-26_234512" -> "10-26 23:45", "...-undo" -> "10-26 23:45 undo"; other names as they are. */
static void display_name(const Backup *backup, char *out, int size) {
    const char *n = backup->name;
    if (strlen(n) >= 17 && n[4] == '-' && n[7] == '-' && n[10] == '_') {
        snprintf(out, size, "%.5s %.2s:%.2s%s", n + 5, n + 11, n + 13, strstr(n, "-undo") != NULL ? " undo" : "");
    } else {
        snprintf(out, size, "%.16s", n);
    }
}

/* Makes backup/<date><suffix> and returns its path under the data folder in out. */
static void make_dated_folder(const char *suffix, char *out, int size) {
    char path[160];
    time_t now = time(NULL);
    struct tm *t = gmtime(&now); // The console's clock is local time.
    snprintf(out, size, BACKUP_DIR "/%04d-%02d-%02d_%02d%02d%02d%s", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec, suffix);
    snprintf(path, sizeof(path), "%s/" BACKUP_DIR, plat_data_dir());
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/%s", plat_data_dir(), out);
    mkdir(path, 0777);
}

static void back_up(void) {
    char dir[64];
    int result;
    if (port_autorun_enabled()) {
        set_status(ROW_BACK_UP, "Not during a test run");
        return;
    }
    make_dated_folder("", dir, sizeof(dir));
    result = port_save_backup(dir);
    if (result > 0) {
        scan();
        sPicked = 0;
        set_status(ROW_BACK_UP, "Backed up to sdmc:/3ds/dkr/backup");
    } else {
        set_status(ROW_BACK_UP, result < 0 ? "The game is busy: try again" : "Could not write to the SD card");
    }
}

/* Reads a backup's file into buffer (size bytes; a longer file, such as a 2 KB .eep, is cut). */
static int read_file(const Backup *backup, const char *file, u8 *buffer, unsigned int size) {
    char path[192];
    snprintf(path, sizeof(path), "%s/" BACKUP_DIR "/%s/%s", plat_data_dir(), backup->name, file);
    return plat_load_file_start(path, buffer, size) == (int) size;
}

static void restore(void) {
    static u8 sEeprom[PORT_EEPROM_SIZE];
    static u8 sPak[PORT_MEMPAK_SIZE];
    const Backup *backup = &sBackups[sPicked];
    int haveEeprom, havePak, result;
    char undo[64];

    if (port_autorun_enabled()) {
        set_status(ROW_RESTORE, "Not during a test run");
        return;
    }
    haveEeprom = backup->eep[0] != '\0' && read_file(backup, backup->eep, sEeprom, sizeof(sEeprom));
    havePak = backup->mpk[0] != '\0' && read_file(backup, backup->mpk, sPak, sizeof(sPak));
    if (!haveEeprom && !havePak) {
        set_status(ROW_RESTORE, "That backup's files are too short");
        return;
    }
    make_dated_folder("-undo", undo, sizeof(undo));
    result = port_save_restore(haveEeprom ? sEeprom : NULL, havePak ? sPak : NULL, undo);
    if (result > 0) {
        set_status(ROW_RESTORE, "Restored: the game closes to load it");
        plat_3ds_request_quit();
    } else {
        set_status(ROW_RESTORE, result < 0 ? "The game is busy: try again" : "Could not write to the SD card");
    }
}

static int armed(void) {
    return sArmedAt != 0 && osGetTime() - sArmedAt < CONFIRM_MS;
}

int saves_option_count(void) {
    return ROW_COUNT;
}

const char *saves_option_label(int i) {
    if (i == ROW_BACK_UP) {
        return "Back up now";
    } else if (i == ROW_PICK) {
        return "Backup";
    }
    return armed() ? "Tap again to restore" : "Restore backup";
}

void saves_option_value(int i, char *out, int size) {
    if (i == ROW_PICK) {
        if (sBackupCount == 0) {
            scan();
        }
        if (sBackupCount == 0) {
            snprintf(out, size, "None");
        } else {
            display_name(&sBackups[sPicked], out, size);
        }
    } else if (size > 0) {
        out[0] = '\0';
    }
}

void saves_option_change(int i, int step) {
    if (i != ROW_RESTORE) {
        sArmedAt = 0;
    }
    sStatusRow = -1;
    if (i == ROW_BACK_UP) {
        back_up();
    } else if (i == ROW_PICK) {
        scan();
        if (sBackupCount > 0) {
            sPicked = (sPicked + step + sBackupCount) % sBackupCount;
        }
    } else if (sBackupCount == 0) {
        set_status(ROW_RESTORE, "No backups yet");
    } else if (!armed()) {
        sArmedAt = osGetTime();
    } else {
        sArmedAt = 0;
        restore();
    }
}

const char *saves_option_hint(int i) {
    static char sHint[64];
    if (i == sStatusRow) {
        return sStatus;
    }
    if (i == ROW_BACK_UP) {
        return "Saves a dated copy, ghosts included";
    } else if (i == ROW_PICK) {
        if (sBackupCount == 0) {
            return "Folders in sdmc:/3ds/dkr/backup";
        }
        snprintf(sHint, sizeof(sHint), "%d of %d in sdmc:/3ds/dkr/backup", sPicked + 1, sBackupCount);
        return sHint;
    }
    return armed() ? "Overwrites your save, then quits" : "Your save is backed up first";
}
