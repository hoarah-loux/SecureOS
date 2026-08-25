#include <fat.h>

static uint32_t read_u32(const uint8_t *buffer, uint32_t offset) {
    return (uint32_t)buffer[offset]
        | ((uint32_t)buffer[offset + 1] << 8)
        | ((uint32_t)buffer[offset + 2] << 16)
        | ((uint32_t)buffer[offset + 3] << 24);
}

/* ============================================================
 * FAT32 mount logic
 * ============================================================
 */

int fat32_read_sector(const fat32_mount_t *mount, uint32_t sector, uint32_t count, uint8_t *buffer)
{
    if (!mount || !mount->device || !buffer) {
        return -1;
    }

    return block_device_read(mount->device, (uint64_t)mount->partition_start_sector + sector, count, buffer);
}

static int fat32_validate_oem_type(const uint8_t fs_type[8]) {
    return (fs_type[0] == 'F'
        && fs_type[1] == 'A'
        && fs_type[2] == 'T'
        && fs_type[3] == '3'
        && fs_type[4] == '2');
}

static int fat32_boot_sector_matches(const fat32_boot_sector_t *boot_sector, const uint8_t *buffer) {
    return boot_sector->boot_sector_sig == 0xAA55
        && boot_sector->bytes_per_sector == (uint16_t)((uint32_t)buffer[0x0B] | ((uint32_t)buffer[0x0C] << 8))
        && boot_sector->sectors_per_cluster == buffer[0x0D]
        && boot_sector->reserved_sectors == (uint16_t)((uint32_t)buffer[0x0E] | ((uint32_t)buffer[0x0F] << 8))
        && boot_sector->num_fats == buffer[0x10]
        && boot_sector->sectors_per_fat_32 == read_u32(buffer, 0x24)
        && boot_sector->root_cluster == read_u32(buffer, 0x2C)
        && boot_sector->fsinfo_sector == (uint16_t)((uint32_t)buffer[0x30] | ((uint32_t)buffer[0x31] << 8))
        && boot_sector->backup_boot_sector == (uint16_t)((uint32_t)buffer[0x32] | ((uint32_t)buffer[0x33] << 8));
}

int fat32_mount(fat32_mount_t *mount, block_device_t *device, uint32_t partition_start_sector)
{
    if (!mount || !device) {
        return -1;
    }

    uint8_t boot_buffer[512];
    if (block_device_read(device, partition_start_sector, 1, boot_buffer) != 0) {
        return -1;
    }

    mount->partition_start_sector = partition_start_sector;
    mount->device = device;

    // Before anything can read a FAT entry through it. The mount may be a
    // stack or .bss object the caller never zeroed, in which case a stale
    // fat_cache_valid would hand out whatever the buffer happens to contain as
    // a FAT sector.
    mount->fat_cache_valid = 0;
    mount->fat_cache_sector = 0;
    mount->boot_sector = *(const fat32_boot_sector_t *)(const void *)boot_buffer;

    if (mount->boot_sector.boot_sector_sig != 0xAA55) {
        return -1;
    }

    if (mount->boot_sector.bytes_per_sector != 512) {
        return -1;
    }

    if (mount->boot_sector.sectors_per_cluster == 0) {
        return -1;
    }

    if (mount->boot_sector.root_cluster < 2) {
        return -1;
    }

    if (mount->boot_sector.sectors_per_fat_32 == 0) {
        return -1;
    }

    if (!fat32_validate_oem_type(mount->boot_sector.fs_type)) {
        return -1;
    }

    if (mount->boot_sector.fs_version != 0) {
        return -1;
    }

    mount->fat_size_sectors = mount->boot_sector.sectors_per_fat_32;
    mount->fat_start_sector = partition_start_sector + mount->boot_sector.reserved_sectors;
    mount->fsinfo_sector = partition_start_sector + mount->boot_sector.fsinfo_sector;
    mount->backup_boot_sector = partition_start_sector + mount->boot_sector.backup_boot_sector;
    mount->root_cluster = mount->boot_sector.root_cluster;
    mount->sectors_per_cluster = mount->boot_sector.sectors_per_cluster;
    mount->total_sectors = mount->boot_sector.total_sectors_16 != 0
        ? mount->boot_sector.total_sectors_16
        : mount->boot_sector.total_sectors_32;
    mount->data_start_sector = mount->fat_start_sector + (mount->boot_sector.num_fats * mount->fat_size_sectors);

    uint32_t root_data_sectors = 0;
    if (mount->total_sectors > mount->data_start_sector) {
        root_data_sectors = mount->total_sectors - mount->data_start_sector;
    }
    mount->cluster_count = root_data_sectors / mount->sectors_per_cluster;
    mount->cluster_size_bytes = mount->sectors_per_cluster * mount->boot_sector.bytes_per_sector;

    uint8_t fsinfo_buffer[512];
    if (fat32_read_sector(mount, mount->boot_sector.fsinfo_sector, 1, fsinfo_buffer) == 0) {
        mount->fsinfo.lead_sig = read_u32(fsinfo_buffer, 0x00);
        mount->fsinfo.struct_sig = read_u32(fsinfo_buffer, 0x1E4);
        mount->fsinfo.free_cluster_count = read_u32(fsinfo_buffer, 0x1E8);
        mount->fsinfo.next_free_cluster = read_u32(fsinfo_buffer, 0x1EC);
        mount->fsinfo.trail_sig = read_u32(fsinfo_buffer, 0x1FC);
    }

    if (mount->fsinfo.lead_sig != 0x41615252 || mount->fsinfo.struct_sig != 0x61417272 || mount->fsinfo.trail_sig != 0xAA550000) {
        return -1;
    }

    if (mount->boot_sector.backup_boot_sector != 0) {
        uint8_t backup_buffer[512];
        if (fat32_read_sector(mount, mount->boot_sector.backup_boot_sector, 1, backup_buffer) != 0) {
            return -1;
        }

        fat32_boot_sector_t backup_boot_sector = *(const fat32_boot_sector_t *)(const void *)backup_buffer;
        if (!fat32_boot_sector_matches(&backup_boot_sector, backup_buffer)) {
            return -1;
        }
    }

    return 0;
}
