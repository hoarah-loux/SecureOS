#include <shell.h>

static const char shell_commands[][16] = {
    "ayo",       // help
    "touchgrass", // exit
    "asf",        // ls 
    "billa",      // cat
    "yoink",      // write
    "oops",       // rm
    "nigger",     // touch
    "aura",       // show file perms
};

// add sigma command as sudo


static int serial_print_dir_entry_shell(const fat32_dir_entry_view_t *entry, void *context) {
    (void)context;

    serial_write_string(entry->name);
    if (entry->attributes & FAT_ATTR_DIRECTORY) {
        serial_write_string("/");
    }
    serial_write_string("\r\n");
    return 0;
}

static void shell_print_help(void) {
    serial_write_string("[Shell] Available commands:\r\n");
    for (size_t i = 0; i < sizeof(shell_commands) / sizeof(shell_commands[0]); i++) {
        serial_write_string("  ");
        serial_write_string(shell_commands[i]);
        serial_write_string("\r\n");
    }
}

void shell_init(shell_ctx_t *ctx, fat32_mount_t *mount) {
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->mount = mount;
}

size_t shell_split_args(char *line, char **argv, size_t max_args) {
    size_t argc = 0;
    char *cursor = line;

    if (!line || !argv || max_args == 0) {
        return 0;
    }

    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
            *cursor = '\0';
            cursor++;
        }

        if (*cursor == '\0') {
            break;
        }

        if (argc < max_args) {
            argv[argc++] = cursor;
        }

        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n') {
            cursor++;
        }
    }

    return argc;
}

static int shell_print_file(fat32_mount_t *mount, const char *path) {
    fat32_file_t file = {0};
    uint8_t buffer[128];
    int open_result = fat32_file_open(mount, path, &file);

    if (open_result != 0) {
        serial_write_string("[Shell] cat: file not found\r\n");
        return 1;
    }

    for (;;) {
        int read_result = fat32_file_read(&file, buffer, (uint32_t)sizeof(buffer));
        if (read_result <= 0) {
            break;
        }

        for (int i = 0; i < read_result; i++) {
            char c = (char)buffer[i];
            if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c <= 0x7E)) {
                serial_write_char(c);
            } else {
                serial_write_char('.');
            }
        }
    }

    serial_write_string("\r\n");
    fat32_file_close(&file);
    return 1;
}

static int shell_create_empty_file(fat32_mount_t *mount, const char *path) {
    fat32_file_t file = {0};
    int create_result = fat32_file_create(mount, path, &file);

    if (create_result != 0) {
        serial_write_string("[Shell] touch: create failed\r\n");
        return 1;
    }

    fat32_file_close(&file);
    serial_write_string("[Shell] file created\r\n");
    return 1;
}

static int shell_write_file(fat32_mount_t *mount, const char *path, char **argv, size_t argc) {
    fat32_file_t file = {0};

    if (fat32_file_create(mount, path, &file) != 0) {
        serial_write_string("[Shell] write: create failed\r\n");
        return 1;
    }

    for (size_t i = 2; i < argc; i++) {
        const char *piece = argv[i];
        if (i > 2) {
            (void)fat32_file_append(&file, (const uint8_t *)" ", 1);
        }
        (void)fat32_file_append(&file, (const uint8_t *)piece, (uint32_t)strlen(piece));
    }

    (void)fat32_file_append(&file, (const uint8_t *)"\n", 1);
    fat32_file_close(&file);
    serial_write_string("[Shell] write complete\r\n");
    return 1;
}

int shell_eval(shell_ctx_t *ctx) {
    const char *cmd;

    if (!ctx || ctx->argc == 0 || !ctx->argv[0] || ctx->argv[0][0] == '\0') {
        return 1;
    }

    cmd = ctx->argv[0];

    if (!strcmp(cmd, shell_commands[0])) {
        shell_print_help();
        return 1;
    }

    if (!strcmp(cmd, shell_commands[1])) {
        serial_write_string("[Shell] Exiting shell...\r\n");
        return 0;
    }

    if (!strcmp(cmd, shell_commands[2])) {
        const char *path = (ctx->argc >= 2) ? ctx->argv[1] : "/";
        serial_write_string("[Shell] Listing: ");
        serial_write_string(path);
        serial_write_string("\r\n");
        (void)fat32_dir_list(ctx->mount, path, serial_print_dir_entry_shell, NULL);
        return 1;
    }

    if (!strcmp(cmd, shell_commands[3])) {
        if (ctx->argc < 2) {
            serial_write_string("[Shell] usage: cat <path>\r\n");
            return 1;
        }

        return shell_print_file(ctx->mount, ctx->argv[1]);
    }

    if (!strcmp(cmd, shell_commands[4])) {
        if (ctx->argc < 3) {
            serial_write_string("[Shell] usage: write <path> <text...>\r\n");
            return 1;
        }

        return shell_write_file(ctx->mount, ctx->argv[1], ctx->argv, ctx->argc);
    }

    if (!strcmp(cmd, shell_commands[5])) {
        if (ctx->argc < 2) {
            serial_write_string("[Shell] usage: rm <path>\r\n");
            return 1;
        }

        if (fat32_file_delete(ctx->mount, ctx->argv[1]) != 0) {
            serial_write_string("[Shell] rm failed\r\n");
        } else {
            serial_write_string("[Shell] removed\r\n");
        }
        return 1;
    }

    if (!strcmp(cmd, shell_commands[6])) {
        if (ctx->argc < 2) {
            serial_write_string("[Shell] usage: touch <path>\r\n");
            return 1;
        }

        return shell_create_empty_file(ctx->mount, ctx->argv[1]);
    }

    if (!strcmp(cmd, shell_commands[7])) {
        if (ctx->argc < 2) {
            serial_write_string("[Shell] usage: stat <path>\r\n");
            return 1;
        }

        fat32_dir_entry_view_t entry = {0};
        if (fat32_path_resolve(ctx->mount, ctx->argv[1], &entry) != 0) {
            serial_write_string("[Shell] stat: not found\r\n");
            return 1;
        }

        serial_write_string("[Shell] name=");
        serial_write_string(entry.name);
        serial_write_string(" size=0x");
        serial_write_hex32(entry.file_size);
        serial_write_string(" attr=0x");
        serial_write_hex32(entry.attributes);
        serial_write_string("\r\n");
        return 1;
    }

    serial_write_string("[Shell] unknown command: ");
    serial_write_string(cmd);
    serial_write_string("\r\n");
    return 1;
}