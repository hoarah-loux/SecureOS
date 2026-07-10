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

int fat32_dir_update_file_metadata(fat32_mount_t *mount, uint32_t start_cluster,
                                   const char *name, uint32_t first_cluster,
                                   uint32_t file_size);

int fat32_dir_create_file_entry(fat32_mount_t *mount, uint32_t start_cluster,
                                const char *name, uint32_t first_cluster,
                                uint32_t file_size, uint8_t attributes);

int fat32_dir_delete_entry(fat32_mount_t *mount, uint32_t start_cluster,
                           const char *name);

#endif /* FAT32_DIR_H */