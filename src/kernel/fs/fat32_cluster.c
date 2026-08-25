#include <fat32_cluster.h>

static uint32_t read_u32_le(const uint8_t *buffer) {
    return (uint32_t)buffer[0]
        | ((uint32_t)buffer[1] << 8)
        | ((uint32_t)buffer[2] << 16)
        | ((uint32_t)buffer[3] << 24);
}

static void write_u32_le(uint8_t *buffer, uint32_t value) {
    buffer[0] = (uint8_t)(value & 0xFFu);
    buffer[1] = (uint8_t)((value >> 8) & 0xFFu);
    buffer[2] = (uint8_t)((value >> 16) & 0xFFu);
    buffer[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t fat32_cluster_last_valid(const fat32_mount_t *mount) {
    return mount->cluster_count + 1u;
}

static int fat32_fsinfo_is_valid(const fat32_mount_t *mount) {
    return mount->fsinfo.lead_sig == 0x41615252u
        && mount->fsinfo.struct_sig == 0x61417272u
        && mount->fsinfo.trail_sig == 0xAA550000u;
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
/*
 * The cache lives in the mount but is not part of its logical state, so it is
 * written through a const pointer: every FAT reader in the tree - the chain
 * walk, the directory scan, the file read path - holds the mount as const, and
 * making them all take it mutably to let a cache fill happen would spread a
 * lie about what those functions do. The mount object itself is never
 * const-qualified, so writing through the cast is defined.
 */
void fat32_fat_cache_invalidate(const fat32_mount_t *mount) {
    if (!mount) {
        return;
    }

    ((fat32_mount_t *)mount)->fat_cache_valid = 0;
}

/*
 * Fill the cache with one FAT sector if it does not already hold it, and hand
 * back a pointer into it. NULL means the read failed.
 */
static const uint8_t *fat32_fat_sector(const fat32_mount_t *mount, uint32_t sector_offset) {
    fat32_mount_t *m = (fat32_mount_t *)mount;

    if (m->fat_cache_valid && m->fat_cache_sector == sector_offset) {
        return m->fat_cache;
    }

    /*
     * Invalidate before the read, not after. A failed read leaves the buffer
     * holding a torn mixture of the old sector and the new one; marking the
     * cache valid only once the data is known good is what keeps that mixture
     * from being served as if it were a FAT.
     */
    m->fat_cache_valid = 0;

    if (block_device_read(m->device,
                          (uint64_t)m->fat_start_sector + sector_offset,
                          1,
                          m->fat_cache) != 0) {
        return NULL;
    }

    m->fat_cache_sector = sector_offset;
    m->fat_cache_valid = 1;
    return m->fat_cache;
}

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

    if (bytes_per_sector != sizeof(mount->fat_cache)) {
        return -1;
    }

    /*
     * A FAT32 entry is four bytes and starts at cluster*4, so it can only
     * straddle a sector boundary if the sector size is not a multiple of four.
     * 512 is, and the cache is sized for 512, so the two-sector case the old
     * code carried cannot arise and is rejected above rather than handled.
     */
    if (offset_in_sector > bytes_per_sector - 4u) {
        return -1;
    }

    const uint8_t *fat_sector = fat32_fat_sector(mount, sector_offset);
    if (fat_sector == NULL) {
        return -1;
    }

    uint32_t raw = read_u32_le(fat_sector + offset_in_sector);
    *next_cluster = raw & 0x0FFFFFFFu;
    return 0;
}

/*
 * FAT32 end-of-chain markers are any value >= 0x0FFFFFF8.
 */
int fat32_cluster_is_end_of_chain(uint32_t cluster_value) {
    return (cluster_value & 0x0FFFFFFF) >= 0x0FFFFFF8;
}

int fat32_cluster_write_fat_entry(fat32_mount_t *mount, uint32_t cluster, uint32_t value) {
    if (!mount || !mount->device || cluster < 2) {
        return -1;
    }

    if (cluster > fat32_cluster_last_valid(mount)) {
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

    for (uint32_t fat_index = 0; fat_index < mount->boot_sector.num_fats; fat_index++) {
        uint32_t fat_base = mount->fat_start_sector + (fat_index * mount->fat_size_sectors);
        if (block_device_read(mount->device,
                              (uint64_t)fat_base + sector_offset,
                              sectors_to_read,
                              fat_buffer) != 0) {
            return -1;
        }

        uint32_t raw = read_u32_le(fat_buffer + offset_in_sector);
        raw = (raw & 0xF0000000u) | (value & 0x0FFFFFFFu);
        write_u32_le(fat_buffer + offset_in_sector, raw);

        if (block_device_write(mount->device,
                               (uint64_t)fat_base + sector_offset,
                               sectors_to_read,
                               fat_buffer) != 0) {
            fat32_fat_cache_invalidate(mount);
            return -1;
        }
    }

    // The sector just rewritten may be the one sitting in the read cache.
    // Dropping it is cheaper than working out whether it is, and this path is
    // rare - allocating a cluster, extending a file - while the read path it
    // protects is the hot one.
    fat32_fat_cache_invalidate(mount);
    return 0;
}

int fat32_cluster_allocate(fat32_mount_t *mount, uint32_t *out_cluster) {
    if (!mount || !out_cluster) {
        return -1;
    }

    uint32_t first = 2u;
    uint32_t last = fat32_cluster_last_valid(mount);
    if (last < first) {
        return -1;
    }

    for (uint32_t cluster = first; cluster <= last; cluster++) {
        uint32_t value;
        if (fat32_cluster_read_fat_entry(mount, cluster, &value) != 0) {
            return -1;
        }

        if ((value & 0x0FFFFFFFu) == 0) {
            if (fat32_cluster_write_fat_entry(mount, cluster, 0x0FFFFFFFu) != 0) {
                return -1;
            }

            if (fat32_fsinfo_is_valid(mount)) {
                if (mount->fsinfo.free_cluster_count != 0xFFFFFFFFu && mount->fsinfo.free_cluster_count > 0u) {
                    mount->fsinfo.free_cluster_count--;
                }

                mount->fsinfo.next_free_cluster = cluster + 1u;
                if (mount->fsinfo.next_free_cluster < 2 || mount->fsinfo.next_free_cluster > last) {
                    mount->fsinfo.next_free_cluster = 2;
                }

                if (fat32_fsinfo_sync(mount) != 0) {
                    return -1;
                }
            }

            *out_cluster = cluster;
            return 0;
        }
    }

    return -1;
}

int fat32_cluster_zero(fat32_mount_t *mount, uint32_t cluster) {
    if (!mount || !mount->device || cluster < 2) {
        return -1;
    }

    uint32_t cluster_sector = fat32_cluster_to_sector(mount, cluster);
    if (cluster_sector == 0) {
        return -1;
    }

    uint32_t bytes_per_sector = mount->boot_sector.bytes_per_sector;
    if (bytes_per_sector > 512u) {
        return -1;
    }

    uint8_t zero_sector[512];
    for (uint32_t i = 0; i < bytes_per_sector; i++) {
        zero_sector[i] = 0;
    }

    for (uint32_t sector_offset = 0; sector_offset < mount->sectors_per_cluster; sector_offset++) {
        if (block_device_write(mount->device,
                               (uint64_t)cluster_sector + sector_offset,
                               1,
                               zero_sector) != 0) {
            return -1;
        }
    }

    return 0;
}

int fat32_cluster_free_chain(fat32_mount_t *mount, uint32_t start_cluster) {
    if (!mount || start_cluster < 2 || start_cluster > fat32_cluster_last_valid(mount)) {
        return -1;
    }

    uint32_t current = start_cluster;
    uint32_t guard = 0;
    uint32_t first_freed = start_cluster;

    while (current >= 2 && current <= fat32_cluster_last_valid(mount)) {
        uint32_t next = 0;
        if (fat32_cluster_read_fat_entry(mount, current, &next) != 0) {
            return -1;
        }

        if (fat32_cluster_write_fat_entry(mount, current, 0) != 0) {
            return -1;
        }

        if (fat32_fsinfo_is_valid(mount) && mount->fsinfo.free_cluster_count != 0xFFFFFFFFu) {
            mount->fsinfo.free_cluster_count++;
        }

        if (next == 0 || next == 1 || fat32_cluster_is_end_of_chain(next)) {
            break;
        }

        current = next;
        guard++;
        if (guard > mount->cluster_count) {
            return -1;
        }
    }

    if (fat32_fsinfo_is_valid(mount)) {
        if (mount->fsinfo.next_free_cluster == 0xFFFFFFFFu || mount->fsinfo.next_free_cluster < 2u
            || first_freed < mount->fsinfo.next_free_cluster) {
            mount->fsinfo.next_free_cluster = first_freed;
        }

        if (fat32_fsinfo_sync(mount) != 0) {
            return -1;
        }
    }

    return 0;
}

int fat32_fsinfo_sync(fat32_mount_t *mount) {
    uint8_t buffer[512];

    if (!mount || !mount->device || !fat32_fsinfo_is_valid(mount)) {
        return -1;
    }

    if (block_device_read(mount->device, mount->fsinfo_sector, 1, buffer) != 0) {
        return -1;
    }

    write_u32_le(buffer + 0x00, mount->fsinfo.lead_sig);
    write_u32_le(buffer + 0x1E4, mount->fsinfo.struct_sig);
    write_u32_le(buffer + 0x1E8, mount->fsinfo.free_cluster_count);
    write_u32_le(buffer + 0x1EC, mount->fsinfo.next_free_cluster);
    write_u32_le(buffer + 0x1FC, mount->fsinfo.trail_sig);

    if (block_device_write(mount->device, mount->fsinfo_sector, 1, buffer) != 0) {
        return -1;
    }

    return 0;
}