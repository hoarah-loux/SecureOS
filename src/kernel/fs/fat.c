#include "fs/fat.h"

/* ============================================================
 * FAT12 Boot Sector Reading
 * ============================================================
 */

/**
 * fat12_init - Initialize FAT12 filesystem
 *
 * Reads and validates the boot sector, then calculates the
 * positions of the FAT, root directory, and data area.
 *
 * The FAT12 layout is:
 *   Sector 0: Boot sector
 *   Sectors 1 to (reserved_sectors-1): Reserved (usually empty)
 *   Sectors reserved_sectors to (reserved_sectors + sectors_per_fat*num_fats - 1): FATs
 *   Next: Root directory (fixed size)
 *   Rest: File data (clusters)
 */
int fat12_init(fat12_fs_t *fs)
{
    if (!fs) {
        return -1;
    }

    /* Read sector 0 (boot sector) into memory */
    uint8_t boot_buffer[512];
    if (fat12_read_sector(0, 1, boot_buffer) != 0) {
        return -1;
    }

    /* Parse boot sector into our structure */
    uint8_t *src = boot_buffer;
    uint8_t *dst = (uint8_t *)&fs->boot_sector;
    for (uint32_t i = 0; i < sizeof(fat12_boot_sector_t); i++) {
        dst[i] = src[i];
    }

    /* Validate boot sector */
    if (fs->boot_sector.boot_sector_sig != 0xAA55) {
        /* Invalid boot sector signature */
        return -1;
    }

    if (fs->boot_sector.bytes_per_sector != 512) {
        /* We only support 512-byte sectors for now */
        return -1;
    }

    /* Calculate sector offsets */
    
    /* FAT starts right after reserved sectors */
    fs->fat_start_sector = fs->boot_sector.reserved_sectors;

    /* Root directory starts after all FATs */
    uint32_t fat_size_sectors = (uint32_t)fs->boot_sector.sectors_per_fat 
                                * fs->boot_sector.num_fats;
    fs->root_dir_start_sector = fs->fat_start_sector + fat_size_sectors;

    /* Data area starts after root directory
     * Root directory size in sectors = (num_entries * 32 bytes) / 512
     */
    uint32_t root_dir_size_bytes = (uint32_t)fs->boot_sector.root_dir_entries * 32;
    uint32_t root_dir_size_sectors = (root_dir_size_bytes + 511) / 512;
    fs->data_start_sector = fs->root_dir_start_sector + root_dir_size_sectors;

    return 0;
}

/**
 * fat12_read_sector - Placeholder for disk I/O
 *
 * This function must be implemented to provide actual disk access.
 * For now, this is a stub that returns -1 (not implemented).
 *
 * TODO: Implement one of:
 *   - ATA/IDE driver (PIO mode)
 *   - AHCI driver
 *   - GRUB disk services via multiboot
 */
int fat12_read_sector(uint32_t sector, uint32_t count, uint8_t *buffer)
{
    (void)sector;  /* unused for now */
    (void)count;   /* unused for now */
    (void)buffer;  /* unused for now */

    /* TODO: Implement disk I/O here */
    return -1;
}
