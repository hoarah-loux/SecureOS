#include <syscall.h>
#include <fs_syscalls.h>
#include <keyboard.h>

// Everything below is out of scope for keyboard/filesystem work (process
// control, memory, time, framebuffer) and is stubbed until its owner wires
// it up. Returns -1 (ENOSYS-equivalent, no errno table yet).
static int64_t sys_stub(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return -1;
}

static const syscall_fn syscall_table[SYS_MAX] = {
    [SYS_read]       = sys_read,
    [SYS_write]      = sys_write,
    [SYS_open]       = sys_open,
    [SYS_close]      = sys_close,
    [SYS_lseek]      = sys_lseek,
    [SYS_getkey]     = sys_getkey,
    [SYS_exit]       = sys_stub,
    [SYS_yield]      = sys_stub,
    [SYS_fork]       = sys_stub,
    [SYS_execve]     = sys_stub,
    [SYS_waitpid]    = sys_stub,
    [SYS_getpid]     = sys_stub,
    [SYS_mmap]       = sys_stub,
    [SYS_munmap]     = sys_stub,
    [SYS_brk]        = sys_stub,
    [SYS_gettime]    = sys_stub,
    [SYS_sleep]      = sys_stub,
    [SYS_fb_present] = sys_stub,
};

void syscall_handler(trap_frame *frame, trap_reason trap_reason) {
    (void)trap_reason;

    if (!frame) {
        return;
    }

    uint64_t nr = frame->rax;
    if (nr >= SYS_MAX || syscall_table[nr] == 0) {
        frame->rax = (uint64_t)-1;
        return;
    }

    int64_t ret = syscall_table[nr](frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9);
    frame->rax = (uint64_t)ret;
}
