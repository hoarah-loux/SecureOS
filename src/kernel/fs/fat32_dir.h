#ifndef FAT32_DIR_H
#define FAT32_DIR_H

#include <stdint.h>

#include "fs/fat.h"

/* FAT32 directory traversal helpers:
 * - read a directory cluster chain
 * - parse directory entries
 * - resolve names within subdirectories
 */

int fat32_dir_iterate(const fat32_mount_t *mount, uint32_t start_cluster,
                      int (*visit)(const fat_direntry_t *entry, void *context),
                      void *context);

int fat32_dir_find(const fat32_mount_t *mount, uint32_t start_cluster,
                   const char *name, fat32_dir_entry_view_t *out_entry);

#endif /* FAT32_DIR_H */