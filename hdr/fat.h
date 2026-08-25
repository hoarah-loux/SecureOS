#ifndef FAT_H
#define FAT_H

#include <stdint.h>

#include <block_device.h>

/* ============================================================
 * FAT32 Boot Sector Structure
 * ============================================================
 * This structure represents the boot sector of a FAT32 filesystem.
 * It is located at sector 0 of the disk.
 * More info can be found at https://wiki.osdev.org/FAT#FAT_32
 */
typedef struct {
    uint8_t  jmp[3];              /* 0x00: Jump instruction (EB 3C 90) */
    uint8_t  oem_id[8];           /* 0x03: OEM identifier */
    uint16_t bytes_per_sector;    /* 0x0B: Bytes per sector (usually 512) */
    uint8_t  sectors_per_cluster; /* 0x0D: Sectors per cluster */
    uint16_t reserved_sectors;    /* 0x0E: Reserved sectors before FAT */
    uint8_t  num_fats;            /* 0x10: Number of FAT copies (usually 2) */
    uint16_t root_dir_entries;    /* 0x11: Root directory entry count (0 for FAT32) */
    uint16_t total_sectors_16;    /* 0x13: Total sectors if < 65536 */
    uint8_t  media_descriptor;    /* 0x15: Media descriptor (0xF0 for floppy) */
    uint16_t sectors_per_fat_16;  /* 0x16: Sectors per FAT (0 for FAT32) */
    uint16_t sectors_per_track;   /* 0x18: Sectors per track */
    uint16_t heads;               /* 0x1A: Number of heads */
    uint32_t hidden_sectors;      /* 0x1C: Hidden sectors */
    uint32_t total_sectors_32;    /* 0x20: Total sectors if total_sectors_16 == 0 */
    uint32_t sectors_per_fat_32;  /* 0x24: Sectors per FAT */
    uint16_t ext_flags;           /* 0x28: Extended flags */
    uint16_t fs_version;          /* 0x2A: Filesystem version */
    uint32_t root_cluster;        /* 0x2C: First cluster of root directory */
    uint16_t fsinfo_sector;       /* 0x30: FSInfo sector number */
    uint16_t backup_boot_sector;  /* 0x32: Backup boot sector number */
    uint8_t  reserved[12];        /* 0x34: Reserved */
    uint8_t  drive_number;        /* 0x40: Drive number */
    uint8_t  reserved1;           /* 0x41: Reserved */
    uint8_t  boot_signature;      /* 0x42: Boot signature (0x29) */
    uint32_t volume_serial;       /* 0x43: Volume serial number */
    uint8_t  volume_label[11];    /* 0x47: Volume label */
    uint8_t  fs_type[8];          /* 0x52: Filesystem type ("FAT32   ") */
    uint8_t  boot_code[420];      /* 0x5A: Boot code */
    uint16_t boot_sector_sig;     /* 0x1FE: Boot sector signature (0xAA55) */
} __attribute__((packed)) fat32_boot_sector_t;

/* ============================================================
 * FAT32 FSInfo Sector Structure
 * ============================================================
 */
typedef struct {
    uint32_t lead_sig;            /* 0x00: Must be 0x41615252 */
    uint8_t  reserved1[480];      /* 0x04: Reserved */
    uint32_t struct_sig;          /* 0x1E4: Must be 0x61417272 */
    uint32_t free_cluster_count;  /* 0x1E8: Last known free cluster count */
    uint32_t next_free_cluster;   /* 0x1EC: Suggested allocation start */
    uint8_t  reserved2[12];       /* 0x1F0: Reserved */
    uint32_t trail_sig;           /* 0x1FC: Must be 0xAA550000 */
} __attribute__((packed)) fat32_fsinfo_t;

/* ============================================================
 * FAT Directory Entry Structure
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
} __attribute__((packed)) fat_direntry_t;

/* File attributes */
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20

typedef struct {
    uint32_t partition_start_sector; /* LBA where the volume begins */
    uint32_t fat_start_sector;       /* LBA where the FAT begins */
    uint32_t fsinfo_sector;          /* LBA of the FSInfo sector */
    uint32_t backup_boot_sector;     /* LBA of the backup boot sector */
    uint32_t root_cluster;           /* First cluster of the root directory */
    uint32_t total_sectors;          /* Total sectors in the volume */
    uint32_t fat_size_sectors;       /* Size of one FAT in sectors */
    uint32_t data_start_sector;      /* LBA where cluster data begins */
    uint32_t sectors_per_cluster;    /* Sectors per cluster */
    uint32_t cluster_count;          /* Number of data clusters */
    uint32_t cluster_size_bytes;     /* Bytes per cluster */
    fat32_boot_sector_t boot_sector; /* Parsed boot sector */
    fat32_fsinfo_t fsinfo;           /* Parsed FSInfo sector */
    block_device_t *device;          /* Backing block device */

    /*
     * One-sector cache of the FAT itself.
     *
     * Following a cluster chain reads one FAT entry per cluster, and a FAT
     * sector holds 128 of them, so an uncached walk re-reads the same sector
     * 128 times in a row - and a read is a polled PIO transfer, not a memory
     * access. Reading a 4 MB file with 512-byte clusters walks 8192 entries,
     * which is 8192 disk reads uncached and 64 with this cache.
     *
     * One entry is enough because chain walks are sequential by nature: the
     * FAT sector for cluster N is almost always the one for cluster N+1.
     *
     * fat_cache_sector is a FAT-relative sector index; fat_cache_valid is what
     * says whether it means anything, since 0 is a legitimate index. The write
     * path invalidates it after modifying a FAT entry rather than trying to
     * patch the copy.
     */
    uint8_t  fat_cache[512];         /* Cached FAT sector contents */
    uint32_t fat_cache_sector;       /* Which FAT sector, relative to fat_start_sector */
    uint8_t  fat_cache_valid;        /* Zero until the first fill, and after any FAT write */
} fat32_mount_t;

/**
 * fat32_fat_cache_invalidate - Drop the cached FAT sector.
 *
 * Call after any write that changes a FAT entry. Takes a const pointer for
 * the same reason the cache is filled through one: the cache is not part of
 * the filesystem's logical state, and every FAT reader in the tree holds the
 * mount by const pointer.
 */
void fat32_fat_cache_invalidate(const fat32_mount_t *mount);

typedef struct {
    char name[13];
    uint32_t first_cluster;
    uint32_t file_size;
    uint8_t attributes;
} fat32_dir_entry_view_t;

/**
 * fat32_mount - Initialize the FAT32 filesystem from a block device.
 *
 * Returns:
 *   0 on success
 *  -1 on error.
 */
int fat32_mount(fat32_mount_t *mount, block_device_t *device, uint32_t partition_start_sector);

/**
 * fat32_read_sector - Read sectors from the mounted volume
 *
 * Reads from the mounted FAT32 device using volume-relative sector numbers.
 *
 * Returns:
 *   0 on success
 *  -1 on error
 */
int fat32_read_sector(const fat32_mount_t *mount, uint32_t sector, uint32_t count, uint8_t *buffer);

#endif /* FAT_H */
