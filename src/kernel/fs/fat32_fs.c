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

    return (int)bytes_read;
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

int fat32_file_open(const fat32_mount_t *mount, const char *path, fat32_file_t *out_file) {
    if (!mount || !path || !out_file) {
        return -1;
    }

    if (fat32_path_resolve(mount, path, &out_file->entry) != 0) {
        return -1;
    }

    out_file->mount = mount;
    out_file->position = 0;
    return 0;
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
    for (uint32_t i = 0; i < sizeof(file->entry.name); i++) {
        file->entry.name[i] = '\0';
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