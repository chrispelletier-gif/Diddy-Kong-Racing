#ifndef PLAT_SAVE_BACKUPS_H
#define PLAT_SAVE_BACKUPS_H

/*
 * Backups of the saves (roadmap O10, Q13), for the Settings menu's Saves page and the game's SAVE OPTIONS screen.
 *
 * The saves are dkr.eep and dkr.mpk in the data folder, in the N64's byte order (os/save_convert.c), so they are the
 * same files an N64 emulator keeps. A backup is a folder in backup/ named by its date; any other folder there holding
 * an .eep and/or an .mpk file (an emulator's save, under any name) can be restored too, which is how saves come over
 * from an emulator. A restore first backs up the saves it replaces (a folder ending "-undo"), then the game closes:
 * it keeps its progress in memory, so it has to start again to read the restored one.
 */

enum {
    SAVE_BACKUP_DONE = 1,
    SAVE_BACKUP_WRITE_FAILED = 0,
    SAVE_BACKUP_BUSY = -1,      // The game didn't take the request (loading).
    SAVE_BACKUP_TEST_RUN = -2,  // Not during a test run, whose races mustn't touch the player's saves.
    SAVE_BACKUP_TOO_SHORT = -3, // The backup's files are too short to be saves.
    SAVE_BACKUP_NOT_FOUND = -4, // The backup isn't in the list (or its folder is gone): list them again.
};

#define SAVE_BACKUPS_MAX 32

typedef struct SaveBackup {
    char name[48]; // The folder's name.
    char eep[64];  // Its EEPROM and Controller Pak files, "" if it has none.
    char mpk[64];
} SaveBackup;

/*
 * A list of the backups. The Settings menu and the game's SAVE OPTIONS screen run on different threads, so each keeps
 * its own: a backup made from one doesn't move the rows of the other.
 */
typedef struct SaveBackupList {
    SaveBackup backups[SAVE_BACKUPS_MAX];
    int count;
} SaveBackupList;

/* Lists the newest SAVE_BACKUPS_MAX backups, newest first, and returns how many there are. */
int save_backups_scan(SaveBackupList *list);
int save_backups_count(const SaveBackupList *list);
/* "2026-10-26_234512" as "10-26 23:45" ("... undo" for the copy a restore made); other names as they are. */
void save_backups_name(const SaveBackupList *list, int i, char *out, int size);
/* Whether it is the copy a restore made of the saves it replaced. */
int save_backups_is_undo(const SaveBackupList *list, int i);

/*
 * Backs up the saves to a new dated folder, or restores backup i of list (after which saving is off, and the caller
 * closes the game with plat_request_quit). onGameThread: called by the game itself (its SAVE OPTIONS screen);
 * otherwise the request is handed to the game's thread and waited for. Both return a SAVE_BACKUP_ result. Neither
 * lists the backups again: the caller does, to show the new one.
 */
int save_backups_back_up(int onGameThread);
int save_backups_restore(const SaveBackupList *list, int i, int onGameThread);

#endif
