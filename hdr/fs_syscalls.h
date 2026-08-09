#pragma once
#include <stdint.h>
#include <fat.h>

// open() flags. Only O_CREAT is understood right now (created if the path is
// missing); everything else is opened read/write via the existing fat32 API.
#define SYS_O_CREAT 0x1

// lseek() whence values, matching the usual read/write/exit libc constants.
#define SYS_SEEK_SET 0
#define SYS_SEEK_CUR 1
#define SYS_SEEK_END 2

// Binds the fd table to the mount used to service open/read/write/lseek/close.
// Must be called once after fat32_mount() succeeds and before any process can
// reach the syscall gate.
void syscall_fs_init(fat32_mount_t *mount);

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
