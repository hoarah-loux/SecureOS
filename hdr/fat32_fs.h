#ifndef FAT32_FS_H
#define FAT32_FS_H

#include <stdint.h>
#include <stddef.h>

#include <fat.h>

/*
 * Where a chain walk got to last time, so the next one can carry on from there
 * instead of starting at the first cluster.
 *
 * cluster == 0 means "nothing known yet"; cluster 0 is never a valid data
 * cluster in FAT32, so no separate valid flag is needed. A hint is only ever
 * written from a position reached by following the chain, so the only way one
 * can be wrong is if the chain itself is rewritten - truncation - which resets
 * it. Appending cannot invalidate a hint, since it only adds indices beyond
 * any the hint can name.
 */
typedef struct {
    uint32_t cluster_index;   /* index within the chain, 0 = first cluster */
    uint32_t cluster;         /* the cluster at that index, or 0 if unknown */
} fat32_chain_hint_t;

typedef struct {
    fat32_mount_t *mount;
    fat32_dir_entry_view_t entry;
    uint32_t position;
    uint32_t parent_cluster;
    char short_name[13];
    fat32_chain_hint_t read_hint;
} fat32_file_t;

typedef int (*fat32_dir_visit_fn)(const fat32_dir_entry_view_t *entry, void *context);

int fat32_path_resolve(const fat32_mount_t *mount, const char *path, fat32_dir_entry_view_t *out_entry);
int fat32_file_open(fat32_mount_t *mount, const char *path, fat32_file_t *out_file);
int fat32_file_create(fat32_mount_t *mount, const char *path, fat32_file_t *out_file);
int fat32_file_read(fat32_file_t *file, uint8_t *buffer, uint32_t count);
int fat32_file_write(fat32_file_t *file, const uint8_t *buffer, uint32_t count);
int fat32_file_append(fat32_file_t *file, const uint8_t *buffer, uint32_t count);
int fat32_file_truncate(fat32_file_t *file, uint32_t new_size);
int fat32_file_delete(fat32_mount_t *mount, const char *path);
int fat32_file_seek(fat32_file_t *file, uint32_t position);
int fat32_file_close(fat32_file_t *file);
int fat32_dir_list(const fat32_mount_t *mount, const char *path, fat32_dir_visit_fn visit, void *context);

#endif /* FAT32_FS_H */