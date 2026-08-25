#pragma once
#include <stdint.h>
#include <fat.h>
#include <p.h>

// open() flags. Only O_CREAT is understood right now (created if the path is
// missing); everything else is opened read/write via the existing fat32 API.
#define SYS_O_CREAT 0x1

// lseek() whence values, matching the usual read/write/exit libc constants.
#define SYS_SEEK_SET 0
#define SYS_SEEK_CUR 1
#define SYS_SEEK_END 2

// How far sys_open() will scan a path from ring3 looking for its terminator.
// Paths here are FAT 8.3 with a handful of components, so this is a bound on
// the scan rather than a description of the filesystem: a "string" that never
// terminates is rejected instead of walking through the caller's address
// space.
#define SYS_PATH_MAX 256

// Binds the fd table to the mount used to service open/read/write/lseek/close.
// Must be called once after fat32_mount() succeeds and before any process can
// reach the syscall gate.
void syscall_fs_init(fat32_mount_t *mount);

typedef struct fs_mmap_backing fs_mmap_backing;

typedef struct fs_mmap_pin_out {
    bool success;
    fs_mmap_backing *backing;
    size_t file_size;
    uint64_t error_code;
} fs_mmap_pin_out;

typedef struct fs_mmap_file_key {
    fat32_mount_t *mount;
    uint32_t parent_cluster;
    uint32_t first_cluster;
    char short_name[13];
    bool valid;
} fs_mmap_file_key;

// All five match `syscall_fn` from syscall.h so they can sit directly in the
// dispatch table:
//   sys_open(path, flags, mode, _, _, _)   -> fd or -1
//   sys_close(fd, _, _, _, _, _)           -> 0 or -1
//   sys_read(fd, buf, count, _, _, _)      -> bytes read or -1
//   sys_write(fd, buf, count, _, _, _)     -> bytes written or -1
//   sys_lseek(fd, offset, whence, _, _, _) -> new position or -1
int64_t sys_open(uint64_t path_ptr, uint64_t flags, uint64_t mode, uint64_t a3, uint64_t a4, uint64_t a5);
int64_t sys_close(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
int64_t sys_read(uint64_t fd, uint64_t buf_ptr, uint64_t count, uint64_t a3, uint64_t a4, uint64_t a5);
int64_t sys_write(uint64_t fd, uint64_t buf_ptr, uint64_t count, uint64_t a3, uint64_t a4, uint64_t a5);
int64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence, uint64_t a3, uint64_t a4, uint64_t a5);


fs_mmap_pin_out fs_mmap_pin_fd(uint64_t fd, proc *p);
uint64_t fs_mmap_ref(fs_mmap_backing *backing);
fs_mmap_file_key fs_mmap_file_key_from_fd(uint64_t fd, proc *p);
uint64_t fs_mmap_backing_matches_key(fs_mmap_backing *backing, const fs_mmap_file_key *key);
int64_t fs_mmap_read(fs_mmap_backing *backing, uint64_t file_offset, uint8_t *dst, uint64_t count);
uint64_t fs_mmap_unpin(fs_mmap_backing *backing);