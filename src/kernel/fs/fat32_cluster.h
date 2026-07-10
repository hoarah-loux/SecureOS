#ifndef FAT32_CLUSTER_H
#define FAT32_CLUSTER_H

#include <stdint.h>

#include "fs/fat.h"

/* FAT32 cluster helpers:
 * - cluster-to-sector translation
 * - FAT entry decoding
 * - end-of-chain detection
 */

uint32_t fat32_cluster_to_sector(const fat32_mount_t *mount, uint32_t cluster);
int fat32_cluster_read_fat_entry(const fat32_mount_t *mount, uint32_t cluster, uint32_t *next_cluster);
int fat32_cluster_is_end_of_chain(uint32_t cluster_value);
int fat32_cluster_write_fat_entry(fat32_mount_t *mount, uint32_t cluster, uint32_t value);
int fat32_cluster_allocate(fat32_mount_t *mount, uint32_t *out_cluster);
int fat32_cluster_zero(fat32_mount_t *mount, uint32_t cluster);
int fat32_cluster_free_chain(fat32_mount_t *mount, uint32_t start_cluster);
int fat32_fsinfo_sync(fat32_mount_t *mount);

#endif /* FAT32_CLUSTER_H */