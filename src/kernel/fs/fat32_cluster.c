#include "fs/fat32_cluster.h"

static uint32_t read_u32_le(const uint8_t *buffer) {
    return (uint32_t)buffer[0]
        | ((uint32_t)buffer[1] << 8)
        | ((uint32_t)buffer[2] << 16)
        | ((uint32_t)buffer[3] << 24);
}

/*
 * Cluster numbers start at 2 in FAT32.
 *
 * cluster -> first sector of that cluster:
 *   data_start_sector + (cluster - 2) * sectors_per_cluster
 */
uint32_t fat32_cluster_to_sector(const fat32_mount_t *mount, uint32_t cluster) {
    if (!mount || cluster < 2) {
        return 0;
    }

    return mount->data_start_sector + (cluster - 2u) * mount->sectors_per_cluster;
}

/*
 * FAT32 stores each FAT entry as 32 bits, but only the low 28 bits matter.
 * A FAT entry gives the next cluster in the chain.
 */
int fat32_cluster_read_fat_entry(const fat32_mount_t *mount, uint32_t cluster, uint32_t *next_cluster) {
    if (!mount || !mount->device || !next_cluster || cluster < 2) {
        return -1;
    }

    if (mount->sectors_per_cluster == 0 || mount->boot_sector.bytes_per_sector == 0) {
        return -1;
    }

    if (cluster > mount->cluster_count + 1u) {
        return -1;
    }

    uint32_t bytes_per_sector = mount->boot_sector.bytes_per_sector;
    uint32_t entry_offset = cluster * 4u;
    uint32_t sector_offset = entry_offset / bytes_per_sector;
    uint32_t offset_in_sector = entry_offset % bytes_per_sector;

    uint8_t fat_buffer[1024];
    uint32_t sectors_to_read = 1;
    if (offset_in_sector > bytes_per_sector - 4u) {
        sectors_to_read = 2;
    }

    if (bytes_per_sector > 512u || sectors_to_read * bytes_per_sector > sizeof(fat_buffer)) {
        return -1;
    }

    if (block_device_read(mount->device,
                          (uint64_t)mount->fat_start_sector + sector_offset,
                          sectors_to_read,
                          fat_buffer) != 0) {
        return -1;
    }

    uint32_t raw = read_u32_le(fat_buffer + offset_in_sector);
    *next_cluster = raw & 0x0FFFFFFFu;
    return 0;
}

/*
 * FAT32 end-of-chain markers are any value >= 0x0FFFFFF8.
 */
int fat32_cluster_is_end_of_chain(uint32_t cluster_value) {
    return (cluster_value & 0x0FFFFFFF) >= 0x0FFFFFF8;
}