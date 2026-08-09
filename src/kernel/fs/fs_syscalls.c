#include <fs_syscalls.h>
#include <fat32_fs.h>
#include <keyboard.h>
#include <serial.h>

// NOTE: no user pointer validation here (buf_ptr/path_ptr are trusted as-is).
// The VMM has no user-mode mapping/copy_from_user path yet (see TODO.md), so
// this is only safe for kernel-mode callers until that lands.

#define FS_FD_BASE 3
#define FS_FD_STDIN 0
#define FS_FD_STDOUT 1
#define FS_FD_STDERR 2
#define FS_MAX_OPEN_FILES 16

typedef struct {
    int in_use;
    fat32_file_t file;
} fs_fd_slot_t;

static fat32_mount_t *g_mount;
static fs_fd_slot_t fd_table[FS_MAX_OPEN_FILES];

void syscall_fs_init(fat32_mount_t *mount) {
    g_mount = mount;
    for (int i = 0; i < FS_MAX_OPEN_FILES; i++) {
        fd_table[i].in_use = 0;
    }
}

static int fd_slot_alloc(void) {
    for (int i = 0; i < FS_MAX_OPEN_FILES; i++) {
        if (!fd_table[i].in_use) {
            return i;
        }
    }
    return -1;
}

static fs_fd_slot_t *fd_slot_get(uint64_t fd) {
    if (fd < FS_FD_BASE) {
        return 0;
    }
    uint64_t index = fd - FS_FD_BASE;
    if (index >= FS_MAX_OPEN_FILES || !fd_table[index].in_use) {
        return 0;
    }
    return &fd_table[index];
}

int64_t sys_open(uint64_t path_ptr, uint64_t flags, uint64_t mode, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)mode; (void)a3; (void)a4; (void)a5;

    const char *path = (const char *)path_ptr;
    if (!g_mount || !path) {
        return -1;
    }

    int slot = fd_slot_alloc();
    if (slot < 0) {
        return -1;
    }

    int rc = fat32_file_open(g_mount, path, &fd_table[slot].file);
    if (rc != 0 && (flags & SYS_O_CREAT)) {
        rc = fat32_file_create(g_mount, path, &fd_table[slot].file);
    }
    if (rc != 0) {
        return -1;
    }

    fd_table[slot].in_use = 1;
    return (int64_t)(FS_FD_BASE + slot);
}

int64_t sys_close(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    fs_fd_slot_t *slot = fd_slot_get(fd);
    if (!slot) {
        return -1;
    }

    int rc = fat32_file_close(&slot->file);
    slot->in_use = 0;
    return rc == 0 ? 0 : -1;
}

int64_t sys_read(uint64_t fd, uint64_t buf_ptr, uint64_t count, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    uint8_t *buf = (uint8_t *)buf_ptr;
    if (!buf) {
        return -1;
    }

    if (fd == FS_FD_STDIN) {
        uint64_t read_count = 0;
        char c;
        while (read_count < count && keyboard_getchar_nonblock(&c)) {
            buf[read_count++] = (uint8_t)c;
        }
        return (int64_t)read_count;
    }

    fs_fd_slot_t *slot = fd_slot_get(fd);
    if (!slot) {
        return -1;
    }

    int rc = fat32_file_read(&slot->file, buf, (uint32_t)count);
    return rc >= 0 ? (int64_t)rc : -1;
}

int64_t sys_write(uint64_t fd, uint64_t buf_ptr, uint64_t count, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    const uint8_t *buf = (const uint8_t *)buf_ptr;
    if (!buf) {
        return -1;
    }

    if (fd == FS_FD_STDOUT || fd == FS_FD_STDERR) {
        for (uint64_t i = 0; i < count; i++) {
            serial_write_char((char)buf[i]);
        }
        return (int64_t)count;
    }

    fs_fd_slot_t *slot = fd_slot_get(fd);
    if (!slot) {
        return -1;
    }

    int rc = fat32_file_write(&slot->file, buf, (uint32_t)count);
    return rc >= 0 ? (int64_t)rc : -1;
}

int64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    fs_fd_slot_t *slot = fd_slot_get(fd);
    if (!slot) {
        return -1;
    }

    int64_t new_pos;
    switch (whence) {
        case SYS_SEEK_SET:
            new_pos = (int64_t)offset;
            break;
        case SYS_SEEK_CUR:
            new_pos = (int64_t)slot->file.position + (int64_t)offset;
            break;
        case SYS_SEEK_END:
            new_pos = (int64_t)slot->file.entry.file_size + (int64_t)offset;
            break;
        default:
            return -1;
    }

    if (new_pos < 0) {
        return -1;
    }

    if (fat32_file_seek(&slot->file, (uint32_t)new_pos) != 0) {
        return -1;
    }

    return new_pos;
}
