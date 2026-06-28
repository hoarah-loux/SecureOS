#ifndef FAT32_FS_H
#define FAT32_FS_H

#include <stdint.h>
#include <stddef.h>

#include "fs/fat.h"

typedef struct {
    const fat32_mount_t *mount;
    fat32_dir_entry_view_t entry;
    uint32_t position;
} fat32_file_t;

typedef int (*fat32_dir_visit_fn)(const fat32_dir_entry_view_t *entry, void *context);

int fat32_path_resolve(const fat32_mount_t *mount, const char *path, fat32_dir_entry_view_t *out_entry);
int fat32_file_open(const fat32_mount_t *mount, const char *path, fat32_file_t *out_file);
int fat32_file_read(fat32_file_t *file, uint8_t *buffer, uint32_t count);
int fat32_file_seek(fat32_file_t *file, uint32_t position);
int fat32_file_close(fat32_file_t *file);
int fat32_dir_list(const fat32_mount_t *mount, const char *path, fat32_dir_visit_fn visit, void *context);

#endif /* FAT32_FS_H */