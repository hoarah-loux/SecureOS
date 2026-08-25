#include <syscall.h>
#include <fs_syscalls.h>
#include <keyboard.h>
#include <scheduler.h>
#include <vmm.h>
#include <handlers.h>
#include <idt.h>
#include <framebuffer.h>
#include <p.h>

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



// Moves the program break, backing newlib's _sbrk() and everything above it.
//
// The heap is one anonymous demand-paged region reserved for the whole life of
// the process at load time (elf_load_process), so this only moves a cursor
// inside it: no mapping work, no region churn, and nothing to undo - which is
// why malloc works without SYS_munmap existing. Untouched pages inside the
// reservation cost nothing until the program writes to them, at which point
// the #PF handler zero-fills one.
//
// brk(0) returns the current break. brk(addr) sets it and returns the new
// value, or the old value on failure - the traditional contract newlib's
// _sbrk() expects, where "did it work" is "did the returned value change".
// Shrinking is allowed and does nothing but move the cursor back; the pages
// stay mapped, which is harmless for a process about to exit.
static int64_t sys_brk(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    scheduler_cpu *cpu = scheduler_this_cpu();
    if (cpu == NULL || cpu->current_thread == NULL) { return -1; }

    proc *p = cpu->current_thread->owning_proc;
    if (p == NULL) { return -1; }

    // A zero base means the reservation was never made, so there is no heap to
    // hand out and no honest break to report.
    if (p->brk_base == 0) { return -1; }

    if (a0 == 0) { return (int64_t)p->brk_current; }

    uintptr_t requested = (uintptr_t)a0;

    if (requested < p->brk_base || requested > p->brk_limit) {
        return (int64_t)p->brk_current;
    }

    p->brk_current = requested;
    return (int64_t)p->brk_current;
}

// Milliseconds since boot.
//
// The tick source is the PIT at 100 Hz (pit_init(100) in kmain), so the
// resolution is 10 ms. That is coarse against doom's 35 Hz frame clock -
// 28.57 ms per frame, which quantises to two or three ticks - but it is
// monotonic and it is what exists; a finer clock is a separate change.
static int64_t sys_gettime(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    return (int64_t)(timer_ticks_get() * TIMER_MS_PER_TICK);
}

// Blocks the calling thread for at least a0 milliseconds.
//
// Two things make this less obvious than it looks.
//
// The first is that it cannot block by itself. A syscall_fn has no trap_frame,
// and blocking means saving one - so as with SYS_getkey_wait, this returns
// SYS_WOULD_BLOCK and syscall_handler does the actual queue work.
//
// The second is that syscall_handler rewinds RIP past the `int 0x80` before
// blocking, so a woken thread re-executes this function from the top. A
// version that recomputed its deadline on every entry would reset the clock
// each time it woke and never return. thread::wake_tick is what distinguishes
// the two cases: zero means a new request, non-zero means this is the
// continuation of one already in flight. It is cleared on the way out so the
// next sleep starts fresh.
//
// Rounding is upward, so a sleep is never shorter than asked for. Waking is
// driven by timer_handler, which cannot run more often than once per tick, so
// a sleep can overshoot by up to one tick as well.
static int64_t sys_sleep(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    scheduler_cpu *cpu = scheduler_this_cpu();
    if (cpu == NULL || cpu->current_thread == NULL) { return -1; }

    thread *t = cpu->current_thread;
    uint64_t now = timer_ticks_get();

    if (t->wake_tick != 0) {
        // Continuation. Either the deadline has passed and the sleep is over,
        // or this was a spurious wake and the thread goes back on the queue.
        if (now >= t->wake_tick) {
            t->wake_tick = 0;
            return 0;
        }
        return SYS_WOULD_BLOCK;
    }

    if (a0 == 0) { return 0; }

    uint64_t ticks = (a0 + (TIMER_MS_PER_TICK - 1)) / TIMER_MS_PER_TICK;

    // A deadline of "now" would be indistinguishable from not sleeping, and
    // wake_tick must be non-zero to mark the sleep as in flight.
    if (ticks == 0) { ticks = 1; }

    t->wake_tick = now + ticks;
    return SYS_WOULD_BLOCK;
}


// Largest source buffer this will look at, per side.
//
// The point is not the size - nothing sane asks for more - but that w and h
// arrive from ring3 as raw 64-bit values, and (uint64_t)w * h * 4 overflows if
// they are allowed to be arbitrary. Bounding them first makes every arithmetic
// check below exact rather than approximately right.
#define FB_PRESENT_MAX_DIM 16384ULL

// Draws a userland pixel buffer on the screen, scaled and centred.
//
// The copy through the kernel is not laziness: the framebuffer is mapped into
// the physmap without PTE_US, and sys_mmap has no device-backed path, so there
// is currently no way to hand a process the real pixels. This is the entire
// graphics API - doom's DG_DrawFrame ends here.
//
// a0 = pointer to w*h pixels, 0x00RRGGBB each, packed and row-major
// a1 = width, a2 = height
// Returns 0, or -1 if the framebuffer is unusable or the buffer does not lie
// entirely inside the caller's own address space.
static int64_t sys_fb_present(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    const struct multiboot_tag_framebuffer *fb = fb_get_runtime();
    if (fb == NULL) { return -1; }

    if (a1 == 0 || a2 == 0 || a1 > FB_PRESENT_MAX_DIM || a2 > FB_PRESENT_MAX_DIM) {
        return -1;
    }

    scheduler_cpu *cpu = scheduler_this_cpu();
    if (cpu == NULL || cpu->current_thread == NULL) { return -1; }

    thread *t = cpu->current_thread;
    if (t->owning_proc == NULL || t->owning_proc->adress_space == NULL) { return -1; }

    vm_space *space = t->owning_proc->adress_space;

    // Bound the buffer inside the caller's own user half.
    //
    // Without this, ring3 passes a kernel address and this function reads it
    // and paints it on the screen. Everything the check needs is unsigned
    // 64-bit and already clamped above, so `first + bytes` cannot wrap.
    uint64_t bytes = (uint64_t)a1 * (uint64_t)a2 * 4ULL;
    uint64_t first = a0;
    uint64_t last  = first + bytes - 1ULL;

    if (first == 0 || last < first || last >= space->user_limit) {
        return -1;
    }

    // Being below user_limit is not the same as being mapped. An address in no
    // region of this space faults in kernel mode, and a #PF the handler cannot
    // resolve is a kernel panic, not a dead process - so the region lookup has
    // to happen here rather than being discovered the hard way.
    //
    // Checking both ends catches a buffer that starts inside a region and runs
    // off it. A caller perverse enough to straddle two adjacent regions with a
    // hole in the middle is not covered; that is a deliberate trade, since the
    // alternative is a lookup per page on the hot path of every frame.
    search_region_list_out first_md = search_region_list(space, (uintptr_t)first);
    if (!first_md.success) { return -1; }

    search_region_list_out last_md = search_region_list(space, (uintptr_t)last);
    if (!last_md.success) { return -1; }

    return fb_blit_scaled(fb, (const uint32_t *)(uintptr_t)first,
                          (uint32_t)a1, (uint32_t)a2);
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
    [SYS_brk]        = KIMG_ALIAS(sys_brk),
    [SYS_gettime]    = KIMG_ALIAS(sys_gettime),
    [SYS_sleep]      = KIMG_ALIAS(sys_sleep),
    [SYS_fb_present] = KIMG_ALIAS(sys_fb_present),
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

    // Syscalls that can block do not block themselves: a syscall_fn receives
    // no trap_frame, and blocking means saving one. They return
    // SYS_WOULD_BLOCK and the work happens here, where the frame exists.
    //
    // Which queue depends on what is being waited for - the keyboard IRQ wakes
    // one kind, the timer the other - but everything after that is identical.
    if (ret == SYS_WOULD_BLOCK && (nr == SYS_getkey_wait || nr == SYS_sleep)) {
        // Rewind past `int 0x80` (2-byte opcode) so the blocked thread
        // re-executes the syscall from scratch on wake, naturally
        // re-checking the keyboard buffer instead of resuming mid-syscall.
        //
        // Re-execution is why a blocking syscall has to be written to be
        // re-entrant. SYS_getkey_wait is trivially so - it just looks at the
        // buffer again. SYS_sleep is not, and carries a wake_tick on the
        // thread to tell a continuation apart from a fresh request.
        frame->rip -= 2;

        scheduler_cpu *cpu = scheduler_this_cpu();
        if (cpu == NULL) { frame->rax = (uint64_t)-1; return; }

        thread *current = cpu->current_thread;
        if (current == NULL) { frame->rax = (uint64_t)-1; return; }

        thread_queue *wait_queue = (nr == SYS_sleep) ? cpu->sleep_queue
                                                     : cpu->keyboard_wait_queue;
        if (wait_queue == NULL) { frame->rax = (uint64_t)-1; return; }

        save_trap_frame_out frame_md = save_trap_frame(frame, current);
        if (!frame_md.success) { frame->rax = (uint64_t)-1; return; }

        if (!scheduler_block_current(cpu->cpu_id, wait_queue)) {
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
