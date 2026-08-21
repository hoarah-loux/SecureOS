#pragma once
#include <stdint.h>
#include <context.h>

// Syscall ABI (int 0x80 gate, not SYSCALL/SYSRET):
//   rax = syscall number (see enum syscall_number)
//   args in rdi, rsi, rdx, r10, r8, r9 (r10 instead of rcx since int does not clobber it)
//   return value written back into rax
#define SYSCALL_VECTOR 0x80

typedef int64_t (*syscall_fn)(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

enum syscall_number {
    SYS_read = 0,
    SYS_write,
    SYS_open,
    SYS_close,
    SYS_lseek,
    SYS_getkey,
    SYS_getkey_wait,
    SYS_exit,
    SYS_yield,
    SYS_fork,
    SYS_execve,
    SYS_waitpid,
    SYS_getpid,
    SYS_mmap,
    SYS_munmap,
    SYS_brk,
    SYS_gettime,
    SYS_sleep,
    SYS_fb_present,
    SYS_MAX,
};

void syscall_handler(trap_frame *frame, trap_reason trap_reason);
