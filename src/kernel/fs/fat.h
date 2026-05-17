#ifndef FAT_H
#define FAT_H

#include <stdint.h>

/* ============================================================
 * FAT12 Boot Sector Structure
 * ============================================================
 * This structure represents the boot sector of a FAT12 filesystem.
 * It is located at sector 0 of the disk.
 * More info can be found at https://wiki.osdev.org/FAT#FAT_12
 */
typedef struct {
    uint8_t  jmp[3];              /* 0x00: Jump instruction (EB 3C 90) */
    uint8_t  oem_id[8];           /* 0x03: OEM identifier */
    uint16_t bytes_per_sector;    /* 0x0B: Bytes per sector (usually 512) */
    uint8_t  sectors_per_cluster; /* 0x0D: Sectors per cluster */
    uint16_t reserved_sectors;    /* 0x0E: Reserved sectors before FAT */
    uint8_t  num_fats;            /* 0x10: Number of FAT copies (usually 2) */
    uint16_t root_dir_entries;    /* 0x11: Root directory entry count */
    uint16_t total_sectors;       /* 0x13: Total sectors (0 if >= 65536) */
    uint8_t  media_descriptor;    /* 0x15: Media descriptor (0xF0 for floppy) */
    uint16_t sectors_per_fat;     /* 0x16: Sectors per FAT */
    uint16_t sectors_per_track;   /* 0x18: Sectors per track */
    uint16_t heads;               /* 0x1A: Number of heads */
    uint32_t hidden_sectors;      /* 0x1C: Hidden sectors */
    uint32_t total_sectors_large; /* 0x20: Total sectors (if total_sectors == 0) */
    uint8_t  drive_number;        /* 0x24: Drive number */
    uint8_t  reserved;            /* 0x25: Reserved */
    uint8_t  boot_signature;      /* 0x26: Boot signature (0x29) */
    uint32_t volume_serial;       /* 0x27: Volume serial number */
    uint8_t  volume_label[11];    /* 0x2B: Volume label */
    uint8_t  fs_type[8];          /* 0x36: Filesystem type ("FAT12   ") */
    uint8_t  boot_code[448];      /* 0x3E: Boot code */
    uint16_t boot_sector_sig;     /* 0x1FE: Boot sector signature (0xAA55) */
} __attribute__((packed)) fat12_boot_sector_t;

/* ============================================================
 * FAT12 Directory Entry Structure
 * ============================================================
 * Each entry in the root directory (and subdirectories) is 32 bytes.
 */
typedef struct {
    uint8_t  name[8];             /* 0x00: Filename (space-padded) */
    uint8_t  ext[3];              /* 0x08: File extension (space-padded) */
    uint8_t  attributes;          /* 0x0B: File attributes (read-only, hidden, etc.) */
    uint8_t  reserved;            /* 0x0C: Reserved */
    uint8_t  creation_time_tenth; /* 0x0D: Creation time (tenths of second) */
    uint16_t creation_time;       /* 0x0E: Creation time (HH:MM:SS) */
    uint16_t creation_date;       /* 0x10: Creation date (YY:MM:DD) */
    uint16_t access_date;         /* 0x12: Last access date */
    uint16_t cluster_high;        /* 0x14: High word of first cluster (FAT32 only, 0 for FAT12) */
    uint16_t write_time;          /* 0x16: Last write time */
    uint16_t write_date;          /* 0x18: Last write date */
    uint16_t cluster_low;         /* 0x1A: Low word of first cluster */
    uint32_t file_size;           /* 0x1C: File size in bytes */
} __attribute__((packed)) fat12_direntry_t;

/* File attributes */
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20

/* ============================================================
 * FAT12 Runtime Filesystem Context
 * ============================================================
 * This structure holds the parsed metadata from the boot sector
 * and is used for all subsequent filesystem operations.
 */
typedef struct {
    fat12_boot_sector_t boot_sector;
    uint32_t fat_start_sector;     /* Sector where the FAT begins */
    uint32_t root_dir_start_sector; /* Sector where root directory begins */
    uint32_t data_start_sector;    /* Sector where file data begins */
} fat12_fs_t;

/* ============================================================
 * FAT12 API Functions
 * ============================================================
 */

/**
 * fat12_init - Initialize the FAT12 filesystem
 *
 * Reads and parses the boot sector, calculates sector offsets,
 * and populates the fat12_fs_t structure.
 *
 * Returns:
 *   0 on success
 *  -1 on error (invalid boot sector, bad signature, etc.)
 */
int fat12_init(fat12_fs_t *fs);

/**
 * fat12_read_sector - Read a single sector from disk
 *
 * This function MUST be implemented to provide disk I/O.
 * It will read 'count' sectors starting from 'sector' into 'buffer'.
 *
 * Returns:
 *   0 on success
 *  -1 on error
 */
int fat12_read_sector(uint32_t sector, uint32_t count, uint8_t *buffer);

#endif /* FAT_H */
