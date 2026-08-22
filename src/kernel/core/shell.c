#include <shell.h>

static const char shell_commands[][16] = {
    "help",       // help
    "exit",       // exit
    "ls",         // ls 
    "cat",        // cat
    "write",      // write
    "rm",         // rm
    "touch",      // touch
    "info",       // show file perms
    "pf",         // induce page fault for handler testing
};

// add sigma command as sudo


static int serial_print_dir_entry_shell(const fat32_dir_entry_view_t *entry, void *context) {
    shell_ctx_t* ctx = (shell_ctx_t*)context;
    console_puts(ctx->con, entry->name);
    if (entry->attributes & FAT_ATTR_DIRECTORY) {
        console_putc(ctx->con, '/');
    }

    console_puts(ctx->con, "  ");
    return 0;
}

static void shell_print_help(shell_ctx_t* ctx) {
    console_puts(ctx->con, "Available commands:\n");
    for (size_t i = 0; i < sizeof(shell_commands) / sizeof(shell_commands[0]); i++) {
        console_puts(ctx->con, "  ");
        console_puts(ctx->con, shell_commands[i]);
        console_newline(ctx->con);
    }
}

void shell_init(shell_ctx_t *ctx, fat32_mount_t *mount, console_t *con) {
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->mount = mount;
    ctx->con = con;
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

static int shell_print_file(shell_ctx_t *ctx) {
    fat32_file_t file = {0};
    uint8_t buffer[128];
    char* path = ctx->argv[1];
    int open_result = fat32_file_open(ctx->mount, path, &file);

    if (open_result != 0) {
        console_puts(ctx->con, "cat: file not found\n");
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
                console_putc(ctx->con, c);
            } else {
                console_putc(ctx->con, '.');
            }
        }
    }

    console_newline(ctx->con);
    fat32_file_close(&file);
    return 1;
}

static int shell_create_empty_file(shell_ctx_t *ctx) {
    fat32_file_t file = {0};
    char* path = ctx->argv[1];
    int create_result = fat32_file_create(ctx->mount, path, &file);

    if (create_result != 0) {
        console_puts(ctx->con, "touch: create failed\n");
        return 1;
    }

    fat32_file_close(&file);
    console_puts(ctx->con, "file created\n");
    return 1;
}

static int shell_write_file(shell_ctx_t *ctx) {
    fat32_file_t file = {0};
    char* path = ctx->argv[1];
    size_t n = 1;

    if (fat32_file_create(ctx->mount, path, &file) != 0) {
        console_puts(ctx->con, "write: create failed\n");
        return 1;
    }

    for (size_t i = 2; i < ctx->argc; i++) {
        const char *piece = ctx->argv[i];
        size_t n = 1;
        if (i > 2) {
            n = fat32_file_append(&file, (const uint8_t *)" ", 1);
        }
        n = fat32_file_append(&file, (const uint8_t *)piece, (uint32_t)strlen(piece));
    }

    n = fat32_file_append(&file, (const uint8_t *)"\n", 1);
    fat32_file_close(&file);
    console_printf(ctx->con, "%u write complete\n", n);
    return 1;
}

static int shell_trigger_page_fault(shell_ctx_t *ctx) {
    (void)ctx;

    console_puts(ctx->con, "Triggering page fault via unmapped VA write...\n");

    // Canonical low-half VA chosen outside current kernel/physmap mappings.
    volatile uint64_t *fault_va = (volatile uint64_t *)0x0000400000000000ULL;
    *fault_va = 0x504641554C54554CULL;

    console_puts(ctx->con, "pf: unexpected return (fault did not trigger)\n");
    return 1;
}

int shell_eval(shell_ctx_t *ctx) {
    const char *cmd;

    if (!ctx || ctx->argc == 0 || !ctx->argv[0] || ctx->argv[0][0] == '\0') {
        return 1;
    }

    cmd = ctx->argv[0];

    if (!strcmp(cmd, shell_commands[0])) {
        shell_print_help(ctx);
        return 1;
    }

    if (!strcmp(cmd, shell_commands[1])) {
        console_puts(ctx->con, "Exiting shell...\n");
        return 0;
    }

    if (!strcmp(cmd, shell_commands[2])) {
        const char *path = (ctx->argc >= 2) ? ctx->argv[1] : "/";
        console_printf(ctx->con, "Listing files: %s\n", path);
        (void)fat32_dir_list(ctx->mount, path, serial_print_dir_entry_shell, ctx);
        console_newline(ctx->con);
        return 1;
    }

    if (!strcmp(cmd, shell_commands[3])) {
        if (ctx->argc < 2) {
            console_puts(ctx->con, "usage: cat <path>\n");
            return 1;
        }

        return shell_print_file(ctx);
    }

    if (!strcmp(cmd, shell_commands[4])) {
        if (ctx->argc < 3) {
            console_puts(ctx->con, "usage: write <path> <text...>\n");
            return 1;
        }

        return shell_write_file(ctx);
    }

    if (!strcmp(cmd, shell_commands[5])) {
        if (ctx->argc < 2) {
            console_puts(ctx->con, "usage: rm <path>\n");
            return 1;
        }

        if (fat32_file_delete(ctx->mount, ctx->argv[1]) != 0) {
            console_puts(ctx->con, "rm failed\n");
        } else {
            console_printf(ctx->con, "%s removed\n", ctx->argv[1]);
        }
        return 1;
    }

    if (!strcmp(cmd, shell_commands[6])) {
        if (ctx->argc < 2) {
            console_puts(ctx->con, "usage: touch <path>\n");
            return 1;
        }

        return shell_create_empty_file(ctx);
    }

    if (!strcmp(cmd, shell_commands[7])) {
        if (ctx->argc < 2) {
            console_puts(ctx->con, "usage: info <path>\n");
            return 1;
        }

        fat32_dir_entry_view_t entry = {0};
        if (fat32_path_resolve(ctx->mount, ctx->argv[1], &entry) != 0) {
            console_puts(ctx->con, "info: not found\n");
            return 1;
        }

        console_printf(ctx->con, "name=%s size=%p attr=%p\n", entry.name, entry.file_size, entry.attributes);
        return 1;
    }

    if (!strcmp(cmd, shell_commands[8])) {
        return shell_trigger_page_fault(ctx);
    }

    console_printf(ctx->con, "unknown command: %s\n", cmd);
    return 1;
}