#include "fs/fat32_dir.h"
#include "fs/fat32_cluster.h"

static uint8_t fat32_to_upper_ascii(uint8_t c) {
    if (c >= 'a' && c <= 'z') {
        return (uint8_t)(c - ('a' - 'A'));
    }
    return c;
}

static int fat32_is_deleted_entry(const fat_direntry_t *entry) {
    return entry->name[0] == 0x00 || entry->name[0] == 0xE5;
}

static int fat32_is_lfn_entry(const fat_direntry_t *entry) {
    return (entry->attributes & 0x0F) == 0x0F;
}

static void fat32_entry_to_name(const fat_direntry_t *entry, char out_name[13]) {
    uint32_t index = 0;

    for (uint32_t i = 0; i < 8 && entry->name[i] != ' '; i++) {
        out_name[index++] = (char)entry->name[i];
    }

    if (entry->ext[0] != ' ' || entry->ext[1] != ' ' || entry->ext[2] != ' ') {
        out_name[index++] = '.';
        for (uint32_t i = 0; i < 3 && entry->ext[i] != ' '; i++) {
            out_name[index++] = (char)entry->ext[i];
        }
    }

    out_name[index] = '\0';
}

static int fat32_name_matches(const fat_direntry_t *entry, const char *name) {
    char expected[12];
    uint32_t expected_index = 0;
    uint32_t name_index = 0;
    int seen_dot = 0;

    if (!entry || !name || name[0] == '\0') {
        return 0;
    }

    for (uint32_t i = 0; i < 11; i++) {
        expected[i] = ' ';
    }

    while (name[name_index] != '\0') {
        char c = name[name_index++];

        if (c == '.') {
            if (seen_dot) {
                return 0;
            }
            seen_dot = 1;
            expected_index = 8;
            continue;
        }

        if (expected_index >= 11) {
            return 0;
        }

        expected[expected_index++] = (char)fat32_to_upper_ascii((uint8_t)c);
    }

    for (uint32_t i = 0; i < 8; i++) {
        if (fat32_to_upper_ascii(entry->name[i]) != (uint8_t)expected[i]) {
            return 0;
        }
    }

    for (uint32_t i = 0; i < 3; i++) {
        if (fat32_to_upper_ascii(entry->ext[i]) != (uint8_t)expected[8 + i]) {
            return 0;
        }
    }

    return 1;
}

static uint32_t fat32_entry_first_cluster(const fat_direntry_t *entry) {
    return ((uint32_t)entry->cluster_high << 16) | (uint32_t)entry->cluster_low;
}

typedef struct {
    const char *name;
    fat32_dir_entry_view_t *out_entry;
    int found;
} fat32_find_context_t;

static int fat32_dir_find_callback(const fat_direntry_t *entry, void *user_context) {
    fat32_find_context_t *ctx = (fat32_find_context_t *)user_context;

    if (!fat32_name_matches(entry, ctx->name)) {
        return 0;
    }

    char resolved_name[13];
    fat32_entry_to_name(entry, resolved_name);

    for (uint32_t i = 0; i < sizeof(ctx->out_entry->name); i++) {
        ctx->out_entry->name[i] = '\0';
    }

    for (uint32_t i = 0; i < sizeof(resolved_name); i++) {
        ctx->out_entry->name[i] = resolved_name[i];
        if (resolved_name[i] == '\0') {
            break;
        }
    }

    ctx->out_entry->first_cluster = fat32_entry_first_cluster(entry);
    ctx->out_entry->file_size = entry->file_size;
    ctx->out_entry->attributes = entry->attributes;
    ctx->found = 1;
    return 1;
}

int fat32_dir_iterate(const fat32_mount_t *mount, uint32_t start_cluster,
                      int (*visit)(const fat_direntry_t *entry, void *context),
                      void *context) {
    if (!mount || !mount->device || !visit || start_cluster < 2) {
        return -1;
    }

    uint32_t current_cluster = start_cluster;
    uint32_t next_cluster = 0;
    uint8_t sector_buffer[512];

    while (current_cluster >= 2 && !fat32_cluster_is_end_of_chain(current_cluster)) {
        uint32_t cluster_sector = fat32_cluster_to_sector(mount, current_cluster);
        if (cluster_sector == 0) {
            return -1;
        }

        for (uint32_t sector_offset = 0; sector_offset < mount->sectors_per_cluster; sector_offset++) {
            if (block_device_read(mount->device, (uint64_t)cluster_sector + sector_offset, 1, sector_buffer) != 0) {
                return -1;
            }

            for (uint32_t entry_index = 0; entry_index < 16; entry_index++) {
                const fat_direntry_t *entry = (const fat_direntry_t *)(const void *)(sector_buffer + (entry_index * sizeof(fat_direntry_t)));

                if (entry->name[0] == 0x00) {
                    return 0;
                }

                if (fat32_is_deleted_entry(entry) || fat32_is_lfn_entry(entry)) {
                    continue;
                }

                int visit_result = visit(entry, context);
                if (visit_result != 0) {
                    return visit_result;
                }
            }
        }

        if (fat32_cluster_read_fat_entry(mount, current_cluster, &next_cluster) != 0) {
            return -1;
        }

        if (next_cluster == 0 || fat32_cluster_is_end_of_chain(next_cluster)) {
            return 0;
        }

        current_cluster = next_cluster;
    }

    return 0;
}

int fat32_dir_find(const fat32_mount_t *mount, uint32_t start_cluster,
                   const char *name, fat32_dir_entry_view_t *out_entry) {
    fat32_find_context_t context = { name, out_entry, 0 };

    if (!mount || !name || !out_entry) {
        return -1;
    }

    int iterate_result = fat32_dir_iterate(mount, start_cluster, fat32_dir_find_callback, &context);
    if (iterate_result > 0 && context.found) {
        return 0;
    }

    return -1;
}