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

#endif /* FAT32_CLUSTER_H */