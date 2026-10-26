/*
 * The save files in the N64's byte order (port.h port_eep_convert, port_mpk_convert).
 *
 * The game and libultra write some of their save data as 16, 32 and 64 bit numbers, which a little-endian console
 * stores the other way round. In memory the saves stay the way this port's game code reads them; dkr.eep and dkr.mpk
 * on the SD card are in the N64's order, the same bytes as a real cartridge's EEPROM and Controller Pak, so they are
 * the .eep and .mpk files N64 emulators (ares, Project64, mupen64plus) read and write.
 *
 * EEPROM (4 Kbit, 64 blocks of 8 bytes): the three save files and the time records are bit streams, written a byte at
 * a time; only block 15, the options and global unlocks, is one u64 (save_data.c read_eeprom_settings).
 *
 * Controller Pak (32 KB, 128 pages of 256 bytes, libultra's file system): page 0 holds four copies of the pak's ID
 * (__OSPackId), pages 1 and 2 the page table and its copy (__OSInode, one 2 byte entry a page), pages 3 and 4 the 16
 * file entries (__OSDir), pages 5 on the files. A page table entry is a {bank, page} pair of bytes when it links to a
 * file's next page, but libultra writes its special values (0, end of file 1, free 3) as a u16, so here those two
 * bytes are swapped and a link's are not. Of the files, DKR's own (game code NDY?, company 0x3459) are converted:
 * DKRACING-ADV and DKRACING-TIMES start with a u32 tag and are bit streams after it; DKRACING-GHOSTS is an index of
 * u16 offsets followed by up to six ghosts, each a header of u16s and the recorded nodes, six s16s each. Other
 * games' files are left as they are.
 */
#include <ultra64.h>
#include "types.h"
#include "port.h"

#define EEPROM_SETTINGS_OFFSET (15 * 8)

#define MPK_PAGE 256
#define MPK_PAGES 128
#define MPK_INODE_OFFSET (1 * MPK_PAGE)
#define MPK_INODE_BACKUP_OFFSET (2 * MPK_PAGE)
#define MPK_DIR_OFFSET (3 * MPK_PAGE)
#define MPK_DIR_ENTRIES 16
#define MPK_DIR_SIZE 32
#define MPK_FIRST_DATA_PAGE 5

#define TAG(a, b, c, d) (((u32) (a) << 24) | ((u32) (b) << 16) | ((u32) (c) << 8) | (u32) (d))
#define DKR_COMPANY 0x3459
#define GHOST_SLOTS 6
#define GHOST_INDEX 4 /* After the u32 tag: 7 entries of {u8 level, u8 vehicle, u16 offset}. */

static void swap_bytes(u8 *p, s32 size) {
    s32 i;
    for (i = 0; i < size / 2; i++) {
        u8 t = p[i];
        p[i] = p[size - 1 - i];
        p[size - 1 - i] = t;
    }
}

static u32 read16(const u8 *p, s32 big) {
    return big ? ((u32) p[0] << 8) | p[1] : ((u32) p[1] << 8) | p[0];
}

static u32 read32(const u8 *p, s32 big) {
    return big ? TAG(p[0], p[1], p[2], p[3]) : TAG(p[3], p[2], p[1], p[0]);
}

static void write16(u8 *p, u32 value, s32 big) {
    p[big ? 0 : 1] = (u8) (value >> 8);
    p[big ? 1 : 0] = (u8) value;
}

/* contpfs.c __osIdCheckSum. */
static u32 id_checksum(const u8 *id, s32 big) {
    u32 sum = 0;
    s32 i;
    for (i = 0; i < 0x1C; i += 2) {
        sum += read16(id + i, big);
    }
    return sum & 0xFFFF;
}

void port_eep_convert(u8 *eeprom) {
    swap_bytes(eeprom + EEPROM_SETTINGS_OFFSET, 8);
}

/*
 * A page table entry's next page in bank 0, or -1 for a special value (end of file, free) or anything else. big:
 * whether the pak is in the N64's order.
 */
static s32 inode_next(const u8 *entry, s32 big) {
    s32 special = big ? (entry[0] == 0 && entry[1] < MPK_FIRST_DATA_PAGE) : entry[1] == 0;
    if (special || entry[0] != 0 || entry[1] < MPK_FIRST_DATA_PAGE || entry[1] >= MPK_PAGES) {
        return -1;
    }
    return entry[1];
}

/* A page table entry (or a file entry's first page) to the other order: only the special values change. */
static void convert_inode_entry(u8 *entry, s32 toN64) {
    s32 special = toN64 ? entry[1] == 0 : (entry[0] == 0 && entry[1] < MPK_FIRST_DATA_PAGE);
    if (special) {
        swap_bytes(entry, 2);
    }
}

static void convert_ghosts(u8 *file, s32 size, s32 big) {
    u32 offsets[GHOST_SLOTS + 1];
    s32 i, j;

    if (size < GHOST_INDEX + (GHOST_SLOTS + 1) * 4) {
        return;
    }
    for (i = 0; i <= GHOST_SLOTS; i++) {
        u8 *entry = file + GHOST_INDEX + i * 4;
        offsets[i] = read16(entry + 2, big);
        swap_bytes(entry + 2, 2);
    }
    for (i = 0; i < GHOST_SLOTS; i++) {
        u8 *ghost = file + offsets[i];
        u32 nodes;
        if (file[GHOST_INDEX + i * 4] == 0xFF || offsets[i] + 8 > (u32) size) {
            continue; // An empty slot (or a broken index).
        }
        // GhostHeader: checksum, character and an unused byte, time, node count.
        nodes = read16(ghost + 6, big);
        swap_bytes(ghost + 0, 2);
        swap_bytes(ghost + 4, 2);
        swap_bytes(ghost + 6, 2);
        if (offsets[i] + 8 + nodes * 12 > (u32) size) {
            continue;
        }
        for (j = 0; j < (s32) nodes * 6; j++) {
            swap_bytes(ghost + 8 + j * 2, 2);
        }
    }
}

/* One of DKR's files, gathered from its pages. */
static void convert_dkr_file(u8 *file, s32 size, s32 big) {
    u32 tag;
    if (size < 4) {
        return;
    }
    tag = read32(file, big);
    if (tag == TAG('G', 'A', 'M', 'D') || tag == TAG('T', 'I', 'M', 'D')) {
        swap_bytes(file, 4);
    } else if (tag == TAG('G', 'H', 'S', 'S')) {
        swap_bytes(file, 4);
        convert_ghosts(file, size, big);
    }
}

/* Follows each of DKR's files through the page table (still in the pak's own order) and converts what it holds. */
static void convert_files(u8 *pak, s32 big) {
    static u8 sFile[MPK_PAGES * MPK_PAGE];
    u8 pages[MPK_PAGES];
    s32 d, count, page, i;

    for (d = 0; d < MPK_DIR_ENTRIES; d++) {
        const u8 *dir = pak + MPK_DIR_OFFSET + d * MPK_DIR_SIZE;
        u32 game = read32(dir, big);
        if ((game != TAG('N', 'D', 'Y', 'E') && game != TAG('N', 'D', 'Y', 'P') && game != TAG('N', 'D', 'Y', 'J')) ||
            read16(dir + 4, big) != DKR_COMPANY) {
            continue;
        }
        count = 0;
        for (page = inode_next(dir + 6, big); page >= 0 && count < MPK_PAGES - MPK_FIRST_DATA_PAGE;
             page = inode_next(pak + MPK_INODE_OFFSET + page * 2, big)) {
            pages[count++] = page;
        }
        for (i = 0; i < count; i++) {
            bcopy(pak + pages[i] * MPK_PAGE, sFile + i * MPK_PAGE, MPK_PAGE);
        }
        convert_dkr_file(sFile, count * MPK_PAGE, big);
        for (i = 0; i < count; i++) {
            bcopy(sFile + i * MPK_PAGE, pak + pages[i] * MPK_PAGE, MPK_PAGE);
        }
    }
}

void port_mpk_convert(u8 *pak, s32 toN64) {
    static const s32 sIdOffsets[4] = { 0x20, 0x60, 0x80, 0xC0 };
    s32 i, t, sum;

    convert_files(pak, !toN64);
    for (i = 0; i < 4; i++) {
        // __OSPackId: repaired, random, serial_mid (u64), serial_low (u64), deviceid, banks, version, checksum and
        // its inverse. The checksum is the sum of the first 14 u16s, so the two bytes banks and version count
        // differently in the other order: a good one is worked out again, a bad one stays bad.
        u8 *id = pak + sIdOffsets[i];
        s32 good = id_checksum(id, !toN64) == read16(id + 0x1C, !toN64) &&
                   ((0xFFF2 - id_checksum(id, !toN64)) & 0xFFFF) == read16(id + 0x1E, !toN64);
        swap_bytes(id + 0x00, 4);
        swap_bytes(id + 0x04, 4);
        swap_bytes(id + 0x08, 8);
        swap_bytes(id + 0x10, 8);
        swap_bytes(id + 0x18, 2);
        swap_bytes(id + 0x1C, 2);
        swap_bytes(id + 0x1E, 2);
        if (good) {
            write16(id + 0x1C, id_checksum(id, toN64), toN64);
            write16(id + 0x1E, 0xFFF2 - id_checksum(id, toN64), toN64);
        }
    }
    for (t = 0; t < 2; t++) {
        u8 *table = pak + (t == 0 ? MPK_INODE_OFFSET : MPK_INODE_BACKUP_OFFSET);
        for (i = 1; i < MPK_PAGES; i++) {
            convert_inode_entry(table + i * 2, toN64);
        }
        // Entry 0's second byte is the sum of the bytes of the data pages' entries (contpfs.c __osPfsRWInode). After
        // a format libultra writes it as a u16, so a little-endian pak can have it in the wrong byte: write it again.
        for (i = MPK_FIRST_DATA_PAGE * 2, sum = 0; i < MPK_PAGE; i++) {
            sum += table[i];
        }
        table[1] = (u8) sum;
    }
    for (i = 0; i < MPK_DIR_ENTRIES; i++) {
        // __OSDir: game code, company code, first page, status, reserved, data sum, extension, name.
        u8 *dir = pak + MPK_DIR_OFFSET + i * MPK_DIR_SIZE;
        swap_bytes(dir + 0x0, 4);
        swap_bytes(dir + 0x4, 2);
        convert_inode_entry(dir + 0x6, toN64);
        swap_bytes(dir + 0xA, 2);
    }
}
