#include <syscall.h>
#include <fs_syscalls.h>
#include <keyboard.h>
#include <scheduler.h>
#include <vmm.h>
#include <handlers.h>
#include <idt.h>

#define KERNEL_IMAGE_BASE 0xFFFFFFFF80000000 //slot 511 in the pml4
#define KIMG_ALIAS(fun) ((syscall_fn)((uintptr_t)(fun) + KERNEL_IMAGE_BASE))

// So `int $EXIT_VECTOR` below tracks hdr/idt.h instead of hardcoding 0x82.
#define STRINGIFY_INNER(x) #x
#define STRINGIFY(x) STRINGIFY_INNER(x)

// Everything below is out of scope for keyboard/filesystem work (process
// control, memory, time, framebuffer) and is stubbed until its owner wires
// it up. Returns -1 (ENOSYS-equivalent, no errno table yet).
static int64_t sys_stub(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return -1;
}

// Terminates the calling thread with status a0.
//
// The teardown already exists behind EXIT_VECTOR (exit_isr -> exit_handler ->
// scheduler_exit_current + restore_context), so this records the status and
// traps into it rather than duplicating the switch. exit_handler() needs a
// trap_frame, which a syscall_fn never receives - going through the gate is
// what produces one.
//
// The `int` is taken from CPL0, so it is a same-privilege trap: no stack
// switch, the frame just nests on the dying thread's kernel stack, which is
// abandoned along with the thread.
//
// Does not return when it works. It returns -1 when exit_handler() bails,
// which it does whenever scheduler_pick_next() finds nothing to run - so a
// process cannot exit unless some other thread is runnable.
static int64_t sys_exit(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    scheduler_cpu *cpu = scheduler_this_cpu();

    cpu->current_thread->exit_status = a0;

    __asm__ volatile("int $" STRINGIFY(EXIT_VECTOR) ::: "memory");

    return -1;
}



static const syscall_fn syscall_table[SYS_MAX] = {
    [SYS_read]       = KIMG_ALIAS(sys_read),
    [SYS_write]      = KIMG_ALIAS(sys_write),
    [SYS_open]       = KIMG_ALIAS(sys_open),
    [SYS_close]      = KIMG_ALIAS(sys_close),
    [SYS_lseek]      = KIMG_ALIAS(sys_lseek),
    [SYS_getkey]     = KIMG_ALIAS(sys_getkey),
    [SYS_getkey_wait] = KIMG_ALIAS(sys_getkey_wait),
    [SYS_exit]       = KIMG_ALIAS(sys_exit),
    [SYS_yield]      = KIMG_ALIAS(sys_stub),
    [SYS_fork]       = KIMG_ALIAS(sys_stub),
    [SYS_execve]     = KIMG_ALIAS(sys_stub),
    [SYS_waitpid]    = KIMG_ALIAS(sys_stub),
    [SYS_getpid]     = KIMG_ALIAS(sys_stub),
    [SYS_mmap]       = KIMG_ALIAS(sys_mmap),
    [SYS_munmap]     = KIMG_ALIAS(sys_stub), // SET THIS TO ACTUAL MUNMAP
    [SYS_brk]        = KIMG_ALIAS(sys_stub),
    [SYS_gettime]    = KIMG_ALIAS(sys_stub),
    [SYS_sleep]      = KIMG_ALIAS(sys_stub),
    [SYS_fb_present] = KIMG_ALIAS(sys_stub),
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

    if (nr == SYS_getkey_wait && ret == SYS_WOULD_BLOCK) {
        // Rewind past `int 0x80` (2-byte opcode) so the blocked thread
        // re-executes the syscall from scratch on wake, naturally
        // re-checking the keyboard buffer instead of resuming mid-syscall.
        frame->rip -= 2;

        scheduler_cpu *cpu = scheduler_this_cpu();
        if (cpu == NULL) { frame->rax = (uint64_t)-1; return; }

        thread *current = cpu->current_thread;
        if (current == NULL) { frame->rax = (uint64_t)-1; return; }

        save_trap_frame_out frame_md = save_trap_frame(frame, current);
        if (!frame_md.success) { frame->rax = (uint64_t)-1; return; }

        if (!scheduler_block_current(cpu->cpu_id, cpu->keyboard_wait_queue)) {
            frame->rax = (uint64_t)-1;
            return;
        }

        thread *next = scheduler_pick_next(cpu->cpu_id);
        if (next == NULL) { return; } // idle thread fallback TODO, matches rest of scheduler.c

        if (!scheduler_set_current(cpu->cpu_id, next)) { return; }

        restore_context(next->context);
    }

    frame->rax = (uint64_t)ret;
}
