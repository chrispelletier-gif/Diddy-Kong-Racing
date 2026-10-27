#ifndef PORT_SAVES_H
#define PORT_SAVES_H

/*
 * The save backup calls (os/os_si.c), in plain C types so the platform code (plat/save_backups.c), which can't include
 * the game's headers, sees the same declarations as port.h does.
 *
 * Save backups are asked for from another thread (the 3DS Settings menu) and done by the game's at its next
 * controller read, or done at once when the game's thread asks (onGameThread: its SAVE OPTIONS screen).
 * port_save_backup writes the saves, as they are now, as dkr.eep and dkr.mpk in dir (a folder under the data folder,
 * which must exist). port_save_restore first backs up to undoDir (if not NULL), then replaces the saves with the N64
 * order images given (NULL: keep that one) and stops saving until the game restarts, as the game still holds the old
 * progress in its memory. If writing the restored saves fails, the saves the game holds are written back and saving
 * carries on. Both return 1 when done, 0 if the files could not be written, -1 if the game did not get to it in time
 * (nothing done).
 */
long port_save_backup(const char *dir, long onGameThread);
long port_save_restore(const unsigned char *eeprom, const unsigned char *pak, const char *undoDir, long onGameThread);
long port_autorun_enabled(void);
#define PORT_EEPROM_SIZE 512
#define PORT_MEMPAK_SIZE 0x8000

#endif
