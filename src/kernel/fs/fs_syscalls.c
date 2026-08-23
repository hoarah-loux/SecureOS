#include <fs_syscalls.h>
#include <fat32_fs.h>
#include <keyboard.h>
#include <serial.h>
#include <t.h>
#include <scheduler.h>
#include <p.h>

// NOTE: no user pointer validation here (buf_ptr/path_ptr are trusted as-is).
// The VMM has no user-mode mapping/copy_from_user path yet (see TODO.md), so
// this is only safe for kernel-mode callers until that lands.





static fat32_mount_t *g_mount;

struct fs_mmap_backing {
    fat32_file_t file;
    size_t file_size;
    uint32_t refcount;
};


static proc *get_p(void){

    scheduler_cpu *cpu = scheduler_this_cpu();
    if(cpu == NULL) return NULL;
    thread *t = scheduler_current(cpu->cpu_id);
    if(t == NULL) return NULL;
    proc *o_p = t->owning_proc;
    

    return o_p;




}

void syscall_fs_init(fat32_mount_t *mount) {
    g_mount = mount;
}



static int fd_slot_alloc(proc *p) {

    if (p == NULL) {
        return -1;
    }


    for (int i = PROC_FD_BASE; i < PROC_MAX_OPEN_FILES; i++) {
        if (!p->fd_table[i].in_use) {
            return i;
        }
    }
    return -1;
}

static proc_fd_slot_t *fd_slot_get(uint64_t fd, proc *p) {
    if (p == NULL) {
        return NULL;
    }
   
    uint64_t index = fd;
    if(index >= PROC_MAX_OPEN_FILES){return NULL;}
      
    if (!p->fd_table[index].in_use) {
        return 0;
    }
    return &p->fd_table[index];
}

int64_t sys_open(uint64_t path_ptr, uint64_t flags, uint64_t mode, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)mode; (void)a3; (void)a4; (void)a5;

    const char *path = (const char *)path_ptr;
    if (!g_mount || !path) {
        return -1;
    }
     
    proc *p = get_p();
    if (p == NULL) {
        return -1;
    }

    int slot = fd_slot_alloc(p);
    if (slot < 0) {
        return -1;
    }

    int rc = fat32_file_open(g_mount, path, &p->fd_table[slot].file);
    if (rc != 0 && (flags & SYS_O_CREAT)) {
        rc = fat32_file_create(g_mount, path, &p->fd_table[slot].file);
    }
    if (rc != 0) {
        return -1;
    }

    p->fd_table[slot].in_use = 1;
    p->fd_table[slot].kind = PROC_FD_FILE;
    return (int64_t)(slot);
}

int64_t sys_close(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    proc *p = get_p();
    if (p == NULL) {
        return -1;
    }
    
    proc_fd_slot_t *slot = fd_slot_get(fd, p);
    if (!slot) {
        return -1;
    }

    if (slot->kind != PROC_FD_FILE) {
        return -1;
    }

    int rc = fat32_file_close(&slot->file);
    slot->in_use = 0;
    slot->kind = PROC_FD_NONE;
    return rc == 0 ? 0 : -1;
}

int64_t sys_read(uint64_t fd, uint64_t buf_ptr, uint64_t count, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    uint8_t *buf = (uint8_t *)buf_ptr;
    if (!buf) {
        return -1;
    }

    proc *p = get_p();
    if (p == NULL) {
        return -1;
    }

    if (fd == PROC_FD_STDIN) {
        uint64_t read_count = 0;
        char c;
        while (read_count < count && keyboard_getchar_nonblock(&c)) {
            buf[read_count++] = (uint8_t)c;
        }
        return (int64_t)read_count;
    }

    proc_fd_slot_t *slot = fd_slot_get(fd, p);
    if (!slot) {
        return -1;
    }

    if (slot->kind != PROC_FD_FILE) {
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
    proc *p = get_p();
    if (p == NULL) {
        return -1;
    }

    if (fd == PROC_FD_STDOUT || fd == PROC_FD_STDERR) {
        for (uint64_t i = 0; i < count; i++) {
            serial_write_char((char)buf[i]);
        }
        return (int64_t)count;
    }

    proc_fd_slot_t *slot = fd_slot_get(fd, p);
    if (!slot) {
        return -1;
    }

    if (slot->kind != PROC_FD_FILE) {
        return -1;
    }

    int rc = fat32_file_write(&slot->file, buf, (uint32_t)count);
    return rc >= 0 ? (int64_t)rc : -1;
}

int64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    proc *p = get_p();
    if (p == NULL) {
        return -1;
    }

    proc_fd_slot_t *slot = fd_slot_get(fd, p);
    if (!slot) {
        return -1;
    }

    if (slot->kind != PROC_FD_FILE) {
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


fs_mmap_pin_out fs_mmap_pin_fd(uint64_t fd) {
    fs_mmap_pin_out out = {0};

    proc *p = get_p();
    if (p == NULL) {
        out.error_code = 1;
        return out;
    }

    proc_fd_slot_t *slot = fd_slot_get(fd, p);
    if (slot == NULL || slot->kind != PROC_FD_FILE) {
        out.error_code = 1;
        return out;
    }

    fs_mmap_backing *backing = kcalloc(1, sizeof(*backing));
    if (backing == NULL) {
        out.error_code = 2;
        return out;
    }

    backing->file = slot->file;
    backing->file_size = (size_t)slot->file.entry.file_size;
    backing->refcount = 1;

    out.success = true;
    out.backing = backing;
    out.file_size = backing->file_size;
    return out;
}

uint64_t fs_mmap_ref(fs_mmap_backing *backing) {
    if (backing == NULL) {
        return 0;
    }

    if (backing->refcount == UINT32_MAX) {
        return 0;
    }

    backing->refcount++;
    return 1;
}

fs_mmap_file_key fs_mmap_file_key_from_fd(uint64_t fd) {
    fs_mmap_file_key key = {0};

    proc *p = get_p();
    if (p == NULL) {
        return key;
    }

    proc_fd_slot_t *slot = fd_slot_get(fd, p);
    if (slot == NULL || slot->kind != PROC_FD_FILE) {
        return key;
    }

    key.mount = slot->file.mount;
    key.parent_cluster = slot->file.parent_cluster;
    key.first_cluster = slot->file.entry.first_cluster;
    for (int i = 0; i < 13; i++) {
        key.short_name[i] = slot->file.short_name[i];
        if (key.short_name[i] == '\0') {
            break;
        }
    }
    key.valid = true;
    return key;
}

uint64_t fs_mmap_backing_matches_key(fs_mmap_backing *backing, const fs_mmap_file_key *key) {
    if (backing == NULL || key == NULL || !key->valid) {
        return 0;
    }

    if (backing->file.mount != key->mount) {
        return 0;
    }

    if (backing->file.parent_cluster != key->parent_cluster) {
        return 0;
    }

    if (backing->file.entry.first_cluster != key->first_cluster) {
        return 0;
    }

    for (int i = 0; i < 13; i++) {
        if (backing->file.short_name[i] != key->short_name[i]) {
            return 0;
        }
        if (backing->file.short_name[i] == '\0') {
            break;
        }
    }

    return 1;
}

int64_t fs_mmap_read(fs_mmap_backing *backing, uint64_t file_offset, uint8_t *dst, uint64_t count) {
    if (backing == NULL || dst == NULL) {
        return -1;
    }

    if (file_offset > UINT32_MAX) {
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    if (file_offset >= backing->file_size) {
        for (uint64_t i = 0; i < count; i++) {
            dst[i] = 0;
        }
        return 0;
    }

    uint64_t max_read = backing->file_size - file_offset;
    uint64_t to_read = count;
    if (to_read > max_read) {
        to_read = max_read;
    }

    if (to_read > UINT32_MAX) {
        return -1;
    }

    if (fat32_file_seek(&backing->file, (uint32_t)file_offset) != 0) {
        return -1;
    }

    int rc = fat32_file_read(&backing->file, dst, (uint32_t)to_read);
    if (rc < 0) {
        return -1;
    }

    uint64_t read_bytes = (uint64_t)rc;
    for (uint64_t i = read_bytes; i < count; i++) {
        dst[i] = 0;
    }

    return (int64_t)read_bytes;
}

uint64_t fs_mmap_unpin(fs_mmap_backing *backing) {
    if (backing == NULL) {
        return 0;
    }

    if (backing->refcount == 0) {
        return 0;
    }

    backing->refcount--;
    if (backing->refcount == 0) {
        if (!kfree(backing)) {
            return 0;
        }
    }

    return 1;
}
// Pins a file for mmap by path instead of by file descriptor.
//
// fs_mmap_pin_fd() reads the fd table of whatever process is *currently
// scheduled*, which is the wrong table for the ELF loader: it populates the
// address space of a process that has not run yet and owns no open fds. This
// opens the file directly off the mount and hands back a backing with a
// refcount of 1, which the caller owns and must fs_mmap_unpin().
fs_mmap_pin_out fs_mmap_pin_path(fat32_mount_t *mount, const char *path) {
    fs_mmap_pin_out out = {0};

    if (mount == NULL) {
        mount = g_mount;
    }

    if (mount == NULL || path == NULL) {
        out.error_code = 1;
        return out;
    }

    fs_mmap_backing *backing = kcalloc(1, sizeof(*backing));
    if (backing == NULL) {
        out.error_code = 2;
        return out;
    }

    if (fat32_file_open(mount, path, &backing->file) != 0) {
        kfree(backing);
        out.error_code = 1;
        return out;
    }

    backing->file_size = (size_t)backing->file.entry.file_size;
    backing->refcount = 1;

    out.success = true;
    out.backing = backing;
    out.file_size = backing->file_size;
    return out;
}
