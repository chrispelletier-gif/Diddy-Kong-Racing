/*
 * The Settings menu's Saves page: back up the saves to a dated folder and restore one (roadmap O10, Q13). The backups
 * themselves are plat/save_backups.c, which the game's SAVE OPTIONS screen uses too.
 */
#include <3ds.h>
#include <stdio.h>

#include "plat/plat.h"
#include "plat/save_backups.h"

#define CONFIRM_MS 5000

enum { ROW_BACK_UP, ROW_PICK, ROW_RESTORE, ROW_COUNT };

static SaveBackupList sList;
static int sScanned;
static int sPicked;      // Of the backups, newest first.
static u64 sArmedAt;     // When Restore was tapped once (0: not), so a second tap restores.
static char sStatus[64]; // What the last action did, shown as the hint of its row.
static int sStatusRow = -1;

static void set_status(int row, const char *text) {
    snprintf(sStatus, sizeof(sStatus), "%s", text);
    sStatusRow = row;
}

static void scan(void) {
    sScanned = 1;
    if (sPicked >= save_backups_scan(&sList)) {
        sPicked = 0;
    }
}

static const char *failure(int result) {
    switch (result) {
        case SAVE_BACKUP_TEST_RUN:
            return "Not during a test run";
        case SAVE_BACKUP_BUSY:
            return "The game is busy: try again";
        case SAVE_BACKUP_TOO_SHORT:
            return "That backup's files are too short";
        case SAVE_BACKUP_NOT_FOUND:
            return "That backup is gone: pick again";
        default:
            return "Could not write to the SD card";
    }
}

static void back_up(void) {
    int result = save_backups_back_up(0);
    if (result == SAVE_BACKUP_DONE) {
        scan();
        sPicked = 0;
        set_status(ROW_BACK_UP, "Backed up to sdmc:/3ds/dkr/backup");
    } else {
        set_status(ROW_BACK_UP, failure(result));
    }
}

static void restore(void) {
    int result = save_backups_restore(&sList, sPicked, 0);
    if (result == SAVE_BACKUP_DONE) {
        plat_request_quit();
    } else if (result == SAVE_BACKUP_NOT_FOUND) {
        scan();
    }
    set_status(ROW_RESTORE, result == SAVE_BACKUP_DONE ? "Restored: the game closes to load it" : failure(result));
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
        if (!sScanned) {
            scan();
        }
        if (save_backups_count(&sList) == 0) {
            snprintf(out, size, "None");
        } else {
            save_backups_name(&sList, sPicked, out, size);
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
        if (save_backups_count(&sList) > 0) {
            sPicked = (sPicked + step + save_backups_count(&sList)) % save_backups_count(&sList);
        }
    } else if (save_backups_count(&sList) == 0) {
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
        if (save_backups_count(&sList) == 0) {
            return "Folders in sdmc:/3ds/dkr/backup";
        }
        snprintf(sHint, sizeof(sHint), port_text("%d of %d in sdmc:/3ds/dkr/backup"), sPicked + 1,
                 save_backups_count(&sList));
        return sHint;
    }
    return armed() ? "Overwrites your save, then quits" : "Your save is backed up first";
}
