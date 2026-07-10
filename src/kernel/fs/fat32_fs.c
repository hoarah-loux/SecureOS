#include "fs/fat32_fs.h"

#include "fs/fat32_cluster.h"
#include "fs/fat32_dir.h"

typedef struct {
    fat32_dir_visit_fn visit;
    void *context;
} fat32_list_context_t;

static uint32_t fat32_min_u32(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

static uint8_t fat32_to_upper_ascii(uint8_t c) {
    if (c >= 'a' && c <= 'z') {
        return (uint8_t)(c - ('a' - 'A'));
    }
    return c;
}

static void fat32_view_from_entry(const fat_direntry_t *entry, fat32_dir_entry_view_t *view) {
    uint32_t index = 0;

    for (uint32_t i = 0; i < sizeof(view->name); i++) {
        view->name[i] = '\0';
    }

    for (uint32_t i = 0; i < 8 && entry->name[i] != ' ' && index < sizeof(view->name) - 1; i++) {
        view->name[index++] = (char)entry->name[i];
    }

    if (entry->ext[0] != ' ' || entry->ext[1] != ' ' || entry->ext[2] != ' ') {
        if (index < sizeof(view->name) - 1) {
            view->name[index++] = '.';
        }

        for (uint32_t i = 0; i < 3 && entry->ext[i] != ' ' && index < sizeof(view->name) - 1; i++) {
            view->name[index++] = (char)entry->ext[i];
        }
    }

    view->name[index] = '\0';
    view->first_cluster = ((uint32_t)entry->cluster_high << 16) | (uint32_t)entry->cluster_low;
    view->file_size = entry->file_size;
    view->attributes = entry->attributes;
}

static int fat32_is_directory(const fat32_dir_entry_view_t *entry) {
    return (entry->attributes & FAT_ATTR_DIRECTORY) != 0;
}

static int fat32_is_root_path(const char *path) {
    return !path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0');
}

static int fat32_copy_component(const char **cursor, char component[13]) {
    uint32_t index = 0;

    while (**cursor == '/') {
        (*cursor)++;
    }

    if (**cursor == '\0') {
        return 0;
    }

    while (**cursor != '\0' && **cursor != '/') {
        if (index >= 12) {
            return -1;
        }

        component[index++] = (char)fat32_to_upper_ascii((uint8_t)**cursor);
        (*cursor)++;
    }

    component[index] = '\0';
    return 1;
}

static int fat32_copy_short_name(char out_name[13], const char *name) {
    uint32_t i = 0;

    if (!out_name || !name || name[0] == '\0') {
        return -1;
    }

    for (; i < 12 && name[i] != '\0'; i++) {
        out_name[i] = name[i];
    }

    if (name[i] != '\0') {
        return -1;
    }

    out_name[i] = '\0';
    return 0;
}

static int fat32_resolve_directory_cluster(const fat32_mount_t *mount, const char *path, uint32_t *out_cluster) {
    const char *cursor = path;
    char component[13];
    fat32_dir_entry_view_t entry;
    uint32_t current_cluster = mount->root_cluster;

    if (!mount || !out_cluster || fat32_is_root_path(path)) {
        return -1;
    }

    while (1) {
        int component_status = fat32_copy_component(&cursor, component);
        if (component_status < 0) {
            return -1;
        }

        if (component_status == 0) {
            *out_cluster = current_cluster;
            return 0;
        }

        if (fat32_dir_find(mount, current_cluster, component, &entry) != 0) {
            return -1;
        }

        while (*cursor == '/') {
            cursor++;
        }

        if (*cursor == '\0') {
            if (!fat32_is_directory(&entry)) {
                return -1;
            }

            *out_cluster = entry.first_cluster;
            return 0;
        }

        if (!fat32_is_directory(&entry) || entry.first_cluster < 2) {
            return -1;
        }

        current_cluster = entry.first_cluster;
    }
}

static int fat32_resolve_parent_and_name(const fat32_mount_t *mount, const char *path,
                                         uint32_t *out_parent_cluster, char out_name[13]) {
    const char *cursor = path;
    char component[13];
    uint32_t current_cluster;

    if (!mount || !path || !out_parent_cluster || !out_name || fat32_is_root_path(path)) {
        return -1;
    }

    current_cluster = mount->root_cluster;

    while (1) {
        fat32_dir_entry_view_t entry;
        int component_status = fat32_copy_component(&cursor, component);
        if (component_status <= 0) {
            return -1;
        }

        while (*cursor == '/') {
            cursor++;
        }

        if (*cursor == '\0') {
            *out_parent_cluster = current_cluster;
            return fat32_copy_short_name(out_name, component);
        }

        if (fat32_dir_find(mount, current_cluster, component, &entry) != 0) {
            return -1;
        }

        if (!fat32_is_directory(&entry) || entry.first_cluster < 2) {
            return -1;
        }

        current_cluster = entry.first_cluster;
    }
}

static int fat32_dir_list_callback(const fat_direntry_t *entry, void *context) {
    fat32_list_context_t *list_context = (fat32_list_context_t *)context;
    fat32_dir_entry_view_t view;

    fat32_view_from_entry(entry, &view);

    if (!list_context->visit) {
        return 0;
    }

    return list_context->visit(&view, list_context->context);
}

static int fat32_read_file_at(const fat32_mount_t *mount, const fat32_dir_entry_view_t *entry,
                              uint32_t offset, uint8_t *buffer, uint32_t count) {
    uint32_t bytes_per_cluster;
    uint32_t cluster;
    uint32_t cluster_skip;
    uint32_t cluster_offset;
    uint32_t bytes_read = 0;

    if (!mount || !entry || !buffer) {
        return -1;
    }

    if (count == 0 || offset >= entry->file_size) {
        return 0;
    }

    if (entry->first_cluster < 2) {
        return -1;
    }

    bytes_per_cluster = mount->cluster_size_bytes;
    if (bytes_per_cluster == 0) {
        return -1;
    }

    count = fat32_min_u32(count, entry->file_size - offset);
    cluster = entry->first_cluster;
    cluster_skip = offset / bytes_per_cluster;
    cluster_offset = offset % bytes_per_cluster;

    while (cluster_skip > 0) {
        uint32_t next_cluster;
        if (fat32_cluster_read_fat_entry(mount, cluster, &next_cluster) != 0) {
            return -1;
        }

        if (fat32_cluster_is_end_of_chain(next_cluster) || next_cluster < 2) {
            return -1;
        }

        cluster = next_cluster;
        cluster_skip--;
    }

    while (bytes_read < count) {
        uint32_t cluster_sector = fat32_cluster_to_sector(mount, cluster);
        uint32_t sector_index = cluster_offset / mount->boot_sector.bytes_per_sector;
        uint32_t offset_in_sector = cluster_offset % mount->boot_sector.bytes_per_sector;
        uint8_t sector_buffer[512];

        if (cluster_sector == 0) {
            return -1;
        }

        while (sector_index < mount->sectors_per_cluster && bytes_read < count) {
            uint32_t bytes_available_in_sector = mount->boot_sector.bytes_per_sector - offset_in_sector;
            uint32_t bytes_remaining = count - bytes_read;
            uint32_t bytes_to_copy = fat32_min_u32(bytes_available_in_sector, bytes_remaining);

            if (block_device_read((block_device_t *)mount->device, (uint64_t)cluster_sector + sector_index, 1, sector_buffer) != 0) {
                return -1;
            }

            for (uint32_t i = 0; i < bytes_to_copy; i++) {
                buffer[bytes_read + i] = sector_buffer[offset_in_sector + i];
            }

            bytes_read += bytes_to_copy;
            cluster_offset += bytes_to_copy;
            offset_in_sector = 0;
            sector_index++;
        }

        if (bytes_read >= count) {
            return (int)bytes_read;
        }

        {
            uint32_t next_cluster;
            if (fat32_cluster_read_fat_entry(mount, cluster, &next_cluster) != 0) {
                return -1;
            }

            if (fat32_cluster_is_end_of_chain(next_cluster) || next_cluster < 2) {
                return -1;
            }

            cluster = next_cluster;
            cluster_offset = 0;
        }
    }

    return (int)bytes_read;
}

static int fat32_ensure_cluster_for_index(fat32_mount_t *mount, uint32_t *first_cluster,
                                          uint32_t cluster_index, uint32_t *out_cluster) {
    uint32_t cluster;

    if (!mount || !first_cluster || !out_cluster) {
        return -1;
    }

    if (*first_cluster < 2) {
        if (fat32_cluster_allocate(mount, first_cluster) != 0) {
            return -1;
        }

        if (fat32_cluster_zero(mount, *first_cluster) != 0) {
            return -1;
        }
    }

    cluster = *first_cluster;

    for (uint32_t i = 0; i < cluster_index; i++) {
        uint32_t next_cluster;
        if (fat32_cluster_read_fat_entry(mount, cluster, &next_cluster) != 0) {
            return -1;
        }

        if (next_cluster < 2 || fat32_cluster_is_end_of_chain(next_cluster)) {
            uint32_t new_cluster;
            if (fat32_cluster_allocate(mount, &new_cluster) != 0) {
                return -1;
            }

            if (fat32_cluster_zero(mount, new_cluster) != 0) {
                return -1;
            }

            if (fat32_cluster_write_fat_entry(mount, cluster, new_cluster) != 0) {
                return -1;
            }

            cluster = new_cluster;
        } else {
            cluster = next_cluster;
        }
    }

    *out_cluster = cluster;
    return 0;
}

static int fat32_truncate_file_internal(fat32_file_t *file, uint32_t new_size) {
    uint32_t bytes_per_cluster;

    if (!file || !file->mount) {
        return -1;
    }

    if (new_size > file->entry.file_size) {
        return -1;
    }

    if (new_size == file->entry.file_size) {
        return 0;
    }

    bytes_per_cluster = file->mount->cluster_size_bytes;
    if (bytes_per_cluster == 0) {
        return -1;
    }

    if (new_size == 0) {
        if (file->entry.first_cluster >= 2) {
            if (fat32_cluster_free_chain(file->mount, file->entry.first_cluster) != 0) {
                return -1;
            }
        }

        file->entry.first_cluster = 0;
        file->entry.file_size = 0;
        if (file->position > 0) {
            file->position = 0;
        }
        return 0;
    }

    {
        uint32_t keep_index = (new_size - 1u) / bytes_per_cluster;
        uint32_t keep_cluster = file->entry.first_cluster;

        if (keep_cluster < 2) {
            return -1;
        }

        for (uint32_t i = 0; i < keep_index; i++) {
            uint32_t next_cluster;
            if (fat32_cluster_read_fat_entry(file->mount, keep_cluster, &next_cluster) != 0) {
                return -1;
            }

            if (next_cluster < 2 || fat32_cluster_is_end_of_chain(next_cluster)) {
                return -1;
            }

            keep_cluster = next_cluster;
        }

        {
            uint32_t old_next;
            if (fat32_cluster_read_fat_entry(file->mount, keep_cluster, &old_next) != 0) {
                return -1;
            }

            if (old_next >= 2 && !fat32_cluster_is_end_of_chain(old_next)) {
                if (fat32_cluster_free_chain(file->mount, old_next) != 0) {
                    return -1;
                }
            }

            if (fat32_cluster_write_fat_entry(file->mount, keep_cluster, 0x0FFFFFFFu) != 0) {
                return -1;
            }
        }
    }

    file->entry.file_size = new_size;
    if (file->position > new_size) {
        file->position = new_size;
    }

    return 0;
}

int fat32_path_resolve(const fat32_mount_t *mount, const char *path, fat32_dir_entry_view_t *out_entry) {
    const char *cursor = path;
    char component[13];
    fat32_dir_entry_view_t entry;
    uint32_t current_cluster;

    if (!mount || !out_entry) {
        return -1;
    }

    if (fat32_is_root_path(path)) {
        for (uint32_t i = 0; i < sizeof(out_entry->name); i++) {
            out_entry->name[i] = '\0';
        }
        out_entry->name[0] = '/';
        out_entry->name[1] = '\0';
        out_entry->first_cluster = mount->root_cluster;
        out_entry->file_size = 0;
        out_entry->attributes = FAT_ATTR_DIRECTORY;
        return 0;
    }

    current_cluster = mount->root_cluster;

    while (1) {
        int component_status = fat32_copy_component(&cursor, component);
        if (component_status < 0) {
            return -1;
        }

        if (component_status == 0) {
            return -1;
        }

        if (fat32_dir_find(mount, current_cluster, component, &entry) != 0) {
            return -1;
        }

        while (*cursor == '/') {
            cursor++;
        }

        if (*cursor == '\0') {
            *out_entry = entry;
            return 0;
        }

        if (!fat32_is_directory(&entry) || entry.first_cluster < 2) {
            return -1;
        }

        current_cluster = entry.first_cluster;
    }
}

int fat32_file_open(fat32_mount_t *mount, const char *path, fat32_file_t *out_file) {
    uint32_t parent_cluster;
    char name[13];

    if (!mount || !path || !out_file) {
        return -1;
    }

    if (fat32_path_resolve(mount, path, &out_file->entry) != 0) {
        return -1;
    }

    if (fat32_is_directory(&out_file->entry)) {
        return -1;
    }

    if (fat32_resolve_parent_and_name(mount, path, &parent_cluster, name) != 0) {
        return -1;
    }

    out_file->mount = mount;
    out_file->position = 0;
    out_file->parent_cluster = parent_cluster;
    if (fat32_copy_short_name(out_file->short_name, name) != 0) {
        return -1;
    }

    return 0;
}

int fat32_file_create(fat32_mount_t *mount, const char *path, fat32_file_t *out_file) {
    uint32_t parent_cluster;
    char name[13];

    if (!mount || !path || !out_file) {
        return -1;
    }

    if (fat32_resolve_parent_and_name(mount, path, &parent_cluster, name) != 0) {
        return -1;
    }

    if (fat32_dir_create_file_entry(mount, parent_cluster, name, 0, 0, FAT_ATTR_ARCHIVE) != 0) {
        return -1;
    }

    return fat32_file_open(mount, path, out_file);
}

int fat32_file_read(fat32_file_t *file, uint8_t *buffer, uint32_t count) {
    int bytes_read;

    if (!file || !buffer || !file->mount) {
        return -1;
    }

    bytes_read = fat32_read_file_at(file->mount, &file->entry, file->position, buffer, count);
    if (bytes_read < 0) {
        return -1;
    }

    file->position += (uint32_t)bytes_read;
    return bytes_read;
}

int fat32_file_write(fat32_file_t *file, const uint8_t *buffer, uint32_t count) {
    uint32_t bytes_written = 0;
    uint32_t bytes_per_cluster;
    uint32_t bytes_per_sector;

    if (!file || !file->mount || !buffer) {
        return -1;
    }

    if ((file->entry.attributes & FAT_ATTR_DIRECTORY) != 0) {
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    bytes_per_cluster = file->mount->cluster_size_bytes;
    bytes_per_sector = file->mount->boot_sector.bytes_per_sector;
    if (bytes_per_cluster == 0 || bytes_per_sector == 0 || bytes_per_sector > 512u) {
        return -1;
    }

    while (bytes_written < count) {
        uint32_t file_offset = file->position + bytes_written;
        uint32_t cluster_index = file_offset / bytes_per_cluster;
        uint32_t cluster_offset = file_offset % bytes_per_cluster;
        uint32_t cluster;
        uint32_t cluster_sector;
        uint32_t sector_index;
        uint32_t offset_in_sector;
        uint32_t bytes_available_in_sector;
        uint32_t bytes_remaining_in_cluster;
        uint32_t bytes_remaining;
        uint32_t bytes_to_copy;
        uint8_t sector_buffer[512];

        if (fat32_ensure_cluster_for_index(file->mount, &file->entry.first_cluster, cluster_index, &cluster) != 0) {
            return -1;
        }

        cluster_sector = fat32_cluster_to_sector(file->mount, cluster);
        if (cluster_sector == 0) {
            return -1;
        }

        sector_index = cluster_offset / bytes_per_sector;
        offset_in_sector = cluster_offset % bytes_per_sector;
        bytes_available_in_sector = bytes_per_sector - offset_in_sector;
        bytes_remaining_in_cluster = bytes_per_cluster - cluster_offset;
        bytes_remaining = count - bytes_written;
        bytes_to_copy = fat32_min_u32(bytes_available_in_sector,
                                      fat32_min_u32(bytes_remaining_in_cluster, bytes_remaining));

        if (block_device_read(file->mount->device,
                              (uint64_t)cluster_sector + sector_index,
                              1,
                              sector_buffer) != 0) {
            return -1;
        }

        for (uint32_t i = 0; i < bytes_to_copy; i++) {
            sector_buffer[offset_in_sector + i] = buffer[bytes_written + i];
        }

        if (block_device_write(file->mount->device,
                               (uint64_t)cluster_sector + sector_index,
                               1,
                               sector_buffer) != 0) {
            return -1;
        }

        bytes_written += bytes_to_copy;
    }

    file->position += bytes_written;
    if (file->position > file->entry.file_size) {
        file->entry.file_size = file->position;
    }

    if (fat32_dir_update_file_metadata(file->mount,
                                       file->parent_cluster,
                                       file->short_name,
                                       file->entry.first_cluster,
                                       file->entry.file_size) != 0) {
        return -1;
    }

    return (int)bytes_written;
}

int fat32_file_append(fat32_file_t *file, const uint8_t *buffer, uint32_t count) {
    if (!file) {
        return -1;
    }

    file->position = file->entry.file_size;

    return fat32_file_write(file, buffer, count);
}

int fat32_file_truncate(fat32_file_t *file, uint32_t new_size) {
    if (!file || !file->mount) {
        return -1;
    }

    if ((file->entry.attributes & FAT_ATTR_DIRECTORY) != 0) {
        return -1;
    }

    if (fat32_truncate_file_internal(file, new_size) != 0) {
        return -1;
    }

    if (fat32_dir_update_file_metadata(file->mount,
                                       file->parent_cluster,
                                       file->short_name,
                                       file->entry.first_cluster,
                                       file->entry.file_size) != 0) {
        return -1;
    }

    return 0;
}

int fat32_file_delete(fat32_mount_t *mount, const char *path) {
    fat32_file_t file;

    if (!mount || !path) {
        return -1;
    }

    if (fat32_file_open(mount, path, &file) != 0) {
        return -1;
    }

    if ((file.entry.attributes & FAT_ATTR_DIRECTORY) != 0) {
        fat32_file_close(&file);
        return -1;
    }

    if (file.entry.first_cluster >= 2) {
        if (fat32_cluster_free_chain(mount, file.entry.first_cluster) != 0) {
            fat32_file_close(&file);
            return -1;
        }
    }

    if (fat32_dir_delete_entry(mount, file.parent_cluster, file.short_name) != 0) {
        fat32_file_close(&file);
        return -1;
    }

    fat32_file_close(&file);
    return 0;
}

int fat32_file_seek(fat32_file_t *file, uint32_t position) {
    if (!file) {
        return -1;
    }

    if (position > file->entry.file_size) {
        return -1;
    }

    file->position = position;
    return 0;
}

int fat32_file_close(fat32_file_t *file) {
    if (!file) {
        return -1;
    }

    file->mount = 0;
    file->position = 0;
    file->parent_cluster = 0;
    for (uint32_t i = 0; i < sizeof(file->entry.name); i++) {
        file->entry.name[i] = '\0';
    }
    for (uint32_t i = 0; i < sizeof(file->short_name); i++) {
        file->short_name[i] = '\0';
    }
    file->entry.first_cluster = 0;
    file->entry.file_size = 0;
    file->entry.attributes = 0;
    return 0;
}

int fat32_dir_list(const fat32_mount_t *mount, const char *path, fat32_dir_visit_fn visit, void *context) {
    fat32_list_context_t list_context;
    uint32_t directory_cluster;

    if (!mount || !visit) {
        return -1;
    }

    if (fat32_is_root_path(path)) {
        directory_cluster = mount->root_cluster;
    } else if (fat32_resolve_directory_cluster(mount, path, &directory_cluster) != 0) {
        return -1;
    }

    list_context.visit = visit;
    list_context.context = context;
    return fat32_dir_iterate(mount, directory_cluster, fat32_dir_list_callback, &list_context);
}
