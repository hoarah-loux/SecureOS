/*
 * kernel.c
 * Minimal kernel entry point for testing GRUB + Multiboot2 boot.
 */

#include <stdint.h>
#include <stddef.h>
#include <multiboot2.h>
#include <serial.h>
#include <framebuffer.h>
#include <multiboot2_utils.h>
#include <idt.h>
#include <block_device.h>
#include <ide_pio.h>
#include <fat32_fs.h>
#include <pic.h>
#include <keyboard.h>
#include <handlers.h>
#include <pmm.h>
#include <vmm.h>
#include <shell.h>
#include <console.h>
#include <loader.h>
#include <syscall.h>
#include <fs_syscalls.h>
#include <heap.h>
#include <scheduler.h>
#include <stdbool.h>

extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];

extern uint64_t transition(uintptr_t *old_root);
extern uintptr_t vmm_query_page2(uintptr_t virtual_adress);
extern uintptr_t _pmm_free_frame(uintptr_t address_to_free);

#define KERNEL_IMAGE_BASE_VA 0xFFFFFFFF80000000ULL
#define TWO_MIB_PAGE_MASK (~((uintptr_t)0x1FFFFF))

#define PIT_INPUT_HZ 1193182u
#define PIT_COMMAND_PORT 0x43u
#define PIT_CHANNEL0_PORT 0x40u

// Everything the shell thread still touches after kmain hands the CPU over.
//
// File-scope on purpose. kmain's last act is restore_context(), which jumps
// onto the shell thread's own kernel stack and never comes back, so kmain's
// frame is gone from that point on - and three separate things hold pointers
// into it: syscall_fs_init() keeps the mount, console_t keeps the framebuffer
// tag, and shell_ctx_t keeps both the mount and the console.
static block_device_t fat32_disk;
static fat32_mount_t mount;
static struct multiboot_tag_framebuffer fb_runtime;
static console_t con;
static shell_ctx_t shell_ctx;

static inline void pit_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void pit_init(uint32_t hz) {
    uint32_t divisor;

    if (hz == 0) {
        return;
    }

    divisor = PIT_INPUT_HZ / hz;
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 0xFFFFu) {
        divisor = 0xFFFFu;
    }

    pit_outb(PIT_COMMAND_PORT, 0x36);
    pit_outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor & 0xFFu));
    pit_outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor >> 8) & 0xFFu));
}

static void report_timer_irq_smoke_test(uint64_t ticks_before, uint64_t ticks_after) {
    printk("[TIMER] ticks before=0x");
    serial_write_hex64(ticks_before);
    printk(" after=0x");
    serial_write_hex64(ticks_after);
    printk("\r\n");
    printk("[TIMER] irq0 smoke test: ");
    printk((ticks_after > ticks_before) ? "PASS\r\n" : "PENDING\r\n");
}

static uintptr_t read_cr3_root_phys(void) {
    uintptr_t cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3) :: "memory");
    return cr3 & ~((uintptr_t)0xFFF);
}

static int serial_print_dir_entry(const fat32_dir_entry_view_t *entry, void* context) {
    (void)context;
    printk("[FAT32] dir: ");
    printk(entry->name);
    printk(" cluster=0x");
    serial_write_hex32(entry->first_cluster);
    printk(" size=0x");
    serial_write_hex32(entry->file_size);
    printk(" attr=0x");
    serial_write_hex32(entry->attributes);
    printk("\r\n");
    return 0;
}

static int buffer_starts_with(const uint8_t *buffer, uint32_t buffer_size, const char *expected) {
    uint32_t i = 0;

    if (!buffer || !expected) {
        return 0;
    }

    while (expected[i] != '\0') {
        if (i >= buffer_size || buffer[i] != (uint8_t)expected[i]) {
            return 0;
        }
        i++;
    }

    return 1;
}

static int buffer_equals(const uint8_t *buffer, uint32_t buffer_size, const uint8_t *expected, uint32_t expected_size) {
    if (!buffer || !expected || buffer_size != expected_size) {
        return 0;
    }

    for (uint32_t i = 0; i < buffer_size; i++) {
        if (buffer[i] != expected[i]) {
            return 0;
        }
    }

    return 1;
}

static void fat32_print_test_result(const char *name, int passed) {
    printk("[FAT32] test ");
    printk(name);
    printk(passed ? ": PASS\r\n" : ": FAIL\r\n");
}

static void syscall_print_test_result(const char *name, int passed) {
    printk("[SYSCALL] test ");
    printk(name);
    printk(passed ? ": PASS\r\n" : ": FAIL\r\n");
}

// Drives the syscall_handler() dispatcher directly with a synthetic
// trap_frame, bypassing `int 0x80`/ring3 (no usermode entry trampoline
// exists yet, see TODO.md). Same rdi/rsi/rdx/r10/r8/r9 arg order the real
// syscall_isr uses, so this exercises the exact dispatch/table/fd-table path
// a real ring3 caller would hit.
static int64_t syscall_test_invoke(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    trap_frame tf = {0};
    tf.rax = nr;
    tf.rdi = a0;
    tf.rsi = a1;
    tf.rdx = a2;
    tf.r10 = a3;
    tf.r8  = a4;
    syscall_handler(&tf, TRAP_REASON_SYSCALL);
    return (int64_t)tf.rax;
}

static void idle_thread_entry(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

// Registers the CPU's idle thread: what scheduler_pick_next() falls back to
// when the ready queue drains. Without one, the last thread to exit takes
// scheduler_exit_current() down with it (pick_next returns NULL, so there is
// nothing to switch to and the exit fails).
//
// The entry address needs no KERNEL_IMAGE_BASE fixup. Taking a function's
// address inside a function compiles to a RIP-relative lea, and by this point
// RIP is already in the higher-half alias, so it resolves there on its own.
// idt.c and syscall.c add the offset by hand only because their tables are
// static initialisers, which have to be link-time constants and therefore
// carry the low 1M address from linker.ld.
static void probe_hex(const char *label, uint64_t value) {
    printk(label);
    serial_write_hex64(value);
    printk("\r\n");
}

// Dumps everything restore_context() will read out of a thread, plus whether
// the top of its kernel stack is mapped *from the space this runs in*. That
// last part is the discriminator: kernel stacks are supposed to land in the
// PML4 slots create_vm_space() copies from kroot, so a mapping made through
// one proc's PML4 should be visible through every other. A zero here means it
// went into that proc's private tables instead and no other address space can
// see it - which is what would make restore_context()'s pushes (line 70, done
// before the CR3 switch on line 128) land nowhere.
static void probe_thread(const char *tag, thread *t) {
    if (t == NULL) {
        printk(tag);
        printk(" <null thread>\r\n");
        return;
    }

    printk(tag);
    printk("\r\n");
    probe_hex("    _rsp_k            =0x", (uint64_t)t->_rsp_k);
    probe_hex("    kstack_va_start   =0x", (uint64_t)t->kernel_stack_va_start);
    probe_hex("    kstack_va_end     =0x", (uint64_t)t->kernel_stack_va_end);

    if (t->context == NULL) {
        printk("    context           =<null>\r\n");
        return;
    }

    probe_hex("    ctx.kernel_rsp    =0x", (uint64_t)t->context->kernel_rsp);
    probe_hex("    ctx.user_rsp      =0x", (uint64_t)t->context->user_rsp);
    probe_hex("    ctx.rip           =0x", (uint64_t)t->context->rip);
    probe_hex("    ctx.cs            =0x", (uint64_t)t->context->cs);
    probe_hex("    ctx.cr3           =0x", (uint64_t)t->context->cr3);

    // Last mapped qword of the stack, i.e. the first thing a push touches.
    if (t->_rsp_k != 0) {
        probe_hex("    PA(_rsp_k - 8)    =0x", vmm_query_page2((uintptr_t)t->_rsp_k - 8));
    }
}

static thread *install_idle_thread(void) {
    create_proc_out idle_proc = create_proc(true);
    if (!idle_proc.success) {
        printk("[SCHED] idle proc create failed\r\n");
        return NULL;
    }

    create_thread_out idle_t = create_thread(idle_proc.proc,
                                             (uintptr_t)idle_thread_entry,
                                             THREAD_F_MAIN_THREAD, 0);
    if (!idle_t.success) {
        printk("[SCHED] idle thread create failed error_code=%u\r\n",
               (unsigned)idle_t.error_code);
        return NULL;
    }

    // Deliberately not scheduler_add_ready(): the idle thread is the fallback
    // pick_next() uses when the ready queue is *empty*, so putting it in the
    // queue would let it compete with real work.
    if (!scheduler_set_idle_thread(0, idle_t.thread)) {
        printk("[SCHED] idle thread registration failed\r\n");
        return NULL;
    }

    printk("[SCHED] idle thread registered (tid=%u)\r\n",
           (unsigned)idle_t.thread->id);
    probe_hex("[PROBE] idle_thread_entry VA =0x", (uint64_t)(uintptr_t)idle_thread_entry);
    probe_thread("[PROBE] idle @ registration", idle_t.thread);

    return idle_t.thread;
}

#ifdef RING3_PREEMPTION_TEST
// Loads two ring3 binaries as separate processes and hands the CPU to the
// first one.
//
// There is no manual context switching here on purpose: the PIT is already
// running at 100Hz and timer_handler() already does the whole
// save_trap_frame -> add_ready -> pick_next -> restore_context sequence at
// quantum expiry. Two runnable threads is the entire setup. Neither binary
// yields or blocks, so interleaved [RING3-A]/[RING3-B] output can only come
// from the timer.
//
// Both then SYS_exit, the ready queue drains, and pick_next() falls through to
// the idle thread - which is the second half of what this exercises.
//
// Never returns: restore_context() abandons kmain's stack, so the shell below
// is unreachable while this is enabled.
static void run_ring3_preemption_test(fat32_mount_t *mount) {
    thread *idle = install_idle_thread();
    if (idle == NULL) {
        return;
    }

    elf_load_out a = elf_load_process(mount, "/RING3.EXE");
    if (!a.success) {
        printk("[PREEMPT] load /RING3.EXE failed error_code=%u\r\n",
               (unsigned)a.error_code);
        return;
    }

    elf_load_out b = elf_load_process(mount, "/RING3P.EXE");
    if (!b.success) {
        printk("[PREEMPT] load /RING3P.EXE failed error_code=%u\r\n",
               (unsigned)b.error_code);
        return;
    }

    // Re-dumped after both procs exist. Two things to compare against the
    // registration dump above: whether idle's kernel_rsp survived (anything
    // that clobbers it shows up here), and whether idle's stack VA sits in the
    // same range as A's and B's - those two switch between each other all day
    // through the exact same code path, so if all three look alike then
    // restore_context() is not treating idle differently and the fault is
    // somewhere else.
    probe_thread("[PROBE] idle @ post-load", idle);
    probe_thread("[PROBE] thread A", a.t);
    probe_thread("[PROBE] thread B", b.t);

    printk("[PREEMPT] pid %u + pid %u ready; quantum=%u ticks @100Hz\r\n",
           (unsigned)a.p->id, (unsigned)b.p->id,
           (unsigned)SCHEDULER_DEFAULT_QUANTUM_TICKS);
    printk("[PREEMPT] expect [RING3-A]/[RING3-B] interleaved, then idle\r\n");

    // elf_load_process() already queued both threads, so this just takes the
    // first one off the ready queue and jumps into it. Everything after is the
    // timer's job.
    thread *first = scheduler_pick_next(0);
    if (first == NULL) {
        printk("[PREEMPT] pick_next found nothing\r\n");
        return;
    }

    restore_context(first->context);
}
#endif // RING3_PREEMPTION_TEST

static void run_syscall_tests(void) {
    printk("[SYSCALL] running syscall dispatch smoke tests\r\n");
    uint8_t scratch[64] = {0};

    static const char msg[] = "[SYSCALL-TEST] hello via SYS_write\r\n";
    int64_t write_ret = syscall_test_invoke(SYS_write, 1, (uint64_t)msg, sizeof(msg) - 1, 0, 0);
    syscall_print_test_result("write stdout", write_ret == (int64_t)(sizeof(msg) - 1));

    int64_t getkey_ret = syscall_test_invoke(SYS_getkey, 0, 0, 0, 0, 0);
    syscall_print_test_result("getkey with no input == -1", getkey_ret == -1);

    int64_t open_ret = syscall_test_invoke(SYS_open, (uint64_t)"/README.TXT", 0, 0, 0, 0);
    syscall_print_test_result("open /README.TXT", open_ret >= 3);

    if (open_ret >= 3) {
        uint64_t fd = (uint64_t)open_ret;

        int64_t read_ret = syscall_test_invoke(SYS_read, fd, (uint64_t)scratch, sizeof(scratch), 0, 0);
        syscall_print_test_result("read /README.TXT",
            read_ret > 0 && buffer_starts_with(scratch, (uint32_t)read_ret, "This is SecureOS FAT32 storage."));

        int64_t seek_ret = syscall_test_invoke(SYS_lseek, fd, 0, SYS_SEEK_SET, 0, 0);
        syscall_print_test_result("lseek back to 0", seek_ret == 0);

        int64_t close_ret = syscall_test_invoke(SYS_close, fd, 0, 0, 0, 0);
        syscall_print_test_result("close fd", close_ret == 0);
    }

    int64_t bad_fd_ret = syscall_test_invoke(SYS_read, 99, (uint64_t)scratch, 1, 0, 0);
    syscall_print_test_result("read on unopened fd == -1", bad_fd_ret == -1);

    int64_t bad_nr_ret = syscall_test_invoke(SYS_MAX, 0, 0, 0, 0, 0);
    syscall_print_test_result("out-of-range syscall number == -1", bad_nr_ret == -1);
}

static void vmm_print_test_result(const char *name, int passed) {
    printk("[VMM] test ");
    printk(name);
    printk(passed ? ": PASS\r\n" : ": FAIL\r\n");
}

static void pmm_print_test_result(const char *name, int passed) {
    printk("[PMM] test ");
    printk(name);
    printk(passed ? ": PASS\r\n" : ": FAIL\r\n");
}

static void run_bitmap_pmm_tests(void) {
    printk("[PMM] running bitmap allocator tests (_pmm_*)\r\n");
    pmm_debug_bitmap_window(0x10, 4);

    uintptr_t frame_a = _pmm_alloc_frame();
    uintptr_t frame_b = _pmm_alloc_frame();

    pmm_print_test_result("bitmap alloc #1 non-zero", frame_a != 0);
    pmm_print_test_result("bitmap alloc #2 non-zero", frame_b != 0);
    pmm_print_test_result("bitmap alloc unique", frame_a != 0 && frame_b != 0 && frame_a != frame_b);

    if (frame_a == 0 && frame_b != 0) {
        printk("[PMM] warning: first alloc returned 0 but second succeeded; frame 0 handling is likely broken\r\n");
    }

    if (frame_a != 0) {
        uintptr_t free_a = _pmm_free_frame(frame_a);
        pmm_print_test_result("bitmap free #1", free_a == 1);

        uintptr_t frame_c = _pmm_alloc_frame();
        pmm_print_test_result("bitmap alloc after free", frame_c == frame_a);

        if (frame_c != 0) {
            pmm_print_test_result("bitmap free recycled", _pmm_free_frame(frame_c) == 1);
        }
    }

    if (frame_b != 0) {
        pmm_print_test_result("bitmap free #2", _pmm_free_frame(frame_b) == 1);
    }

    uintptr_t invalid_free = _pmm_free_frame((uintptr_t)(pmm_max_phys_addr() + 4096));
    pmm_print_test_result("bitmap reject invalid free", invalid_free == 0);
}

static void draw_boot_banner(const struct multiboot_tag_framebuffer *fb) {
    if (!fb) {
        return;
    }

    if (fb->common.framebuffer_type == 1 && (fb->common.framebuffer_bpp == 32 || fb->common.framebuffer_bpp == 24 || fb->common.framebuffer_bpp == 16)) {
        fb_draw_string(fb, 16, 16, "KERNEL RUNNING!", 0x00FFFFFFu);
    }
}

// `int $SYSCALL_VECTOR` below tracks hdr/syscall.h instead of hardcoding 0x80.
#define STRINGIFY_INNER(x) #x
#define STRINGIFY(x) STRINGIFY_INNER(x)

// Blocking keyboard read for the shell thread.
//
// Goes through the int 0x80 gate rather than calling sys_getkey_wait()
// directly, because only half of that syscall lives in the function. The
// block-and-resume half is in syscall_handler(): it needs a trap_frame to
// save this thread's context before scheduler_block_current() takes it off
// the CPU, and it rewinds RIP by the two bytes of `int 0x80` so the wake path
// re-executes the syscall and re-checks the buffer from scratch. A plain C
// call has neither a frame nor a way back.
//
// Blocking instead of spinning on keyboard_getchar_nonblock() is what lets
// the CPU reach the idle thread's hlt at all: a spinning shell is always
// runnable, so scheduler_pick_next() would never fall through to idle.
//
// The gate is DPL=3 so ring3 can reach it, but CPL0 may take it too. A
// same-privilege `int` does not switch stacks, so the frame just nests on
// this thread's own kernel stack.
static char shell_wait_key(void) {
    for (;;) {
        int64_t ret;

        __asm__ volatile("int $" STRINGIFY(SYSCALL_VECTOR)
                         : "=a"(ret)
                         : "a"((uint64_t)SYS_getkey_wait)
                         : "memory");

        if (ret >= 0) {
            return (char)(unsigned char)ret;
        }

        // -1 is syscall_handler bailing out (nothing runnable to switch to).
        // Retry rather than feed a bogus character into the line.
    }
}

// The console shell, as a kernel thread.
//
// Hoisted out of kmain so that something schedulable owns the loop. kmain's
// stack is abandoned at the handoff at the end of kmain and can never be
// returned to, so the loop had to move somewhere that has a saved context of
// its own.
//
// This goes on the ready queue, not into scheduler_set_idle_thread(). Both
// arrangements satisfy "control comes back to the shell when nothing else is
// runnable", but the idle thread is only picked when the ready queue is
// *empty*, which would starve the shell for as long as a launched program is
// alive - no keystroke would be seen until that program exited. On the ready
// queue the shell gets its own quantum alongside whatever it started.
//
// Never returns: a thread entry point has no caller to return to.
static void shell_thread_entry(void) {
    char line_buffer[SHELL_MAX_LINE];
    size_t index = 0;

    printk("[SHELL] shell thread running\r\n");
    line_buffer[0] = '\0';
    console_puts(&con, con.prompt);

    for (;;) {
        char c = shell_wait_key();

        if (c == '\r' || c == '\n') {
            line_buffer[index] = '\0';
            console_newline(&con);

            char *argv[16] = {0};
            shell_ctx.argc = shell_split_args(line_buffer, argv, 16);
            shell_ctx.argv = argv;
            (void)shell_eval(&shell_ctx);

            index = 0;
            line_buffer[0] = '\0';
            console_puts(&con, con.prompt);
        } else if (c == '\b') {
            if (index > 0) {
                console_backspace(&con);
                line_buffer[index] = '\0';
                index--;
            }
        } else if (index < sizeof(line_buffer) - 1) {
            line_buffer[index++] = c;
            console_putc(&con, c);
        }

        // A full line silently drops further characters. kmain's version
        // ended its loop instead, which for a thread entry point would mean
        // returning to nowhere.
    }
}

// Creates the shell's kernel thread and enqueues it.
//
// create_proc(true) sets PROC_F_KERNEL, which is what lets create_thread()
// accept user_stack_size = 0 - a kernel thread never runs at CPL3 and has no
// user stack to reserve. Its context gets cs = 0x08, so restore_context()
// takes the .build_kernel_iret_frame path.
//
// The entry address needs no KERNEL_IMAGE_BASE fixup, for the same reason
// install_idle_thread()'s does not: taking a function's address inside a
// function compiles to a RIP-relative lea, and RIP is already in the
// higher-half alias by this point.
static thread *install_shell_thread(void) {
    create_proc_out shell_proc = create_proc(true);
    if (!shell_proc.success) {
        printk("[SHELL] shell proc create failed\r\n");
        return NULL;
    }

    create_thread_out shell_t = create_thread(shell_proc.proc,
                                              (uintptr_t)shell_thread_entry,
                                              THREAD_F_MAIN_THREAD, 0);
    if (!shell_t.success) {
        printk("[SHELL] shell thread create failed error_code=%u\r\n",
               (unsigned)shell_t.error_code);
        return NULL;
    }

    sched_add_ready_out ready_md = scheduler_add_ready(0, shell_t.thread);
    if (!ready_md.success) {
        printk("[SHELL] shell thread enqueue failed error_code=%u\r\n",
               (unsigned)ready_md.error_code);
        return NULL;
    }

    printk("[SHELL] shell thread ready (tid=%u)\r\n",
           (unsigned)shell_t.thread->id);
    probe_thread("[PROBE] shell @ registration", shell_t.thread);

    return shell_t.thread;
}

/*
 * Main kernel entry point.
 * Called from src/boot/grub/entry64_mb2.asm as:
 *   kmain(magic, info_ptr)
 * with SysV AMD64 ABI args in RDI/RSI.
 */
void kmain(uint32_t mb2_magic, uint32_t mb2_info_ptr) {
    struct multiboot_tag_framebuffer fb_snapshot = {0};
    uint64_t timer_ticks_boot = 0;
    int fb_snapshot_valid = 0;
    int fb_runtime_valid = 0;

    serial_init();
    printk("[SecureOS] kmain reached\r\n");
   
    {
        const struct multiboot_tag_framebuffer *fb_pre =
            (const struct multiboot_tag_framebuffer *)mb2_find_tag(mb2_info_ptr, MULTIBOOT_TAG_TYPE_FRAMEBUFFER);
        if (fb_pre && fb_pre->common.size >= sizeof(struct multiboot_tag_framebuffer_common)) {
            fb_snapshot = *fb_pre;
            fb_snapshot_valid = 1;
        } else if (fb_pre) {
            printk("[SecureOS] framebuffer tag too small\r\n");
        }
    }

    printk("[VMM] initializing PMM/VMM test path\r\n");
    pmm_init(mb2_info_ptr, (uintptr_t)&_kernel_start, (uintptr_t)&_kernel_end);
    run_bitmap_pmm_tests();

    uintptr_t *boot_root = (uintptr_t *)read_cr3_root_phys();
    uint64_t init_ok = vmm_init(boot_root);
    vmm_print_test_result("vmm_init", init_ok == 1);

    uintptr_t kmain_va = (uintptr_t)&kmain;
    uintptr_t kernel_low_window_start_va = ((uintptr_t)&_kernel_start) & TWO_MIB_PAGE_MASK;
    uintptr_t kernel_low_window_end_va =
        (((uintptr_t)&_kernel_end) + 0x1FFFFF) & ~((uintptr_t)0x1FFFFF);
    uintptr_t kernel_low_window_pages = 0;
    uintptr_t kernel_start_pa_pre = (uintptr_t)vmm_query_page((uintptr_t)&_kernel_start);
    uintptr_t kernel_end_last_pa_pre = (uintptr_t)vmm_query_page(((uintptr_t)&_kernel_end) - 1);
    uintptr_t low_identity_unmap_start_va = 0;
    uintptr_t low_identity_unmap_end_va = 0;
    uintptr_t kmain_higher_half_va = 0;
    if (kmain_va >= kernel_low_window_start_va) {
        kmain_higher_half_va = (uintptr_t)(KERNEL_IMAGE_BASE_VA + (kmain_va - kernel_low_window_start_va));
    }

    if (kernel_low_window_end_va > kernel_low_window_start_va) {
        kernel_low_window_pages =
            (kernel_low_window_end_va - kernel_low_window_start_va) / 4096;
    }

    uintptr_t before_transition_pa = (uintptr_t)vmm_query_page(kmain_va);

    printk("[VMM] kmain VA=0x");
    serial_write_hex64(kmain_va);
    printk(" pre-transition PA=0x");
    serial_write_hex64(before_transition_pa);
    printk("\r\n");
    vmm_print_test_result("query kmain via vmm_query_page", before_transition_pa != 0);


    uint64_t transition_ok = transition(boot_root);
    vmm_print_test_result("transition", transition_ok == 1);

    if (transition_ok == 1 && kernel_start_pa_pre != 0 && kernel_end_last_pa_pre != 0) {
        uintptr_t kernel_end_pa_exclusive = kernel_end_last_pa_pre + 1;

        if (kernel_end_pa_exclusive > kernel_end_last_pa_pre) {
            low_identity_unmap_start_va = kernel_start_pa_pre & TWO_MIB_PAGE_MASK;
            low_identity_unmap_end_va =
                (kernel_end_pa_exclusive + 0x1FFFFF) & ~((uintptr_t)0x1FFFFF);
        } else {
            printk("[VMM] low identity unmap skipped: end overflow\r\n");
        }
    } else {
        printk("[VMM] low identity unmap skipped: preconditions failed\r\n");
    }

    if (low_identity_unmap_end_va > low_identity_unmap_start_va) {
        uintptr_t low_window_pages =
            (low_identity_unmap_end_va - low_identity_unmap_start_va) / 4096;

        if (kernel_low_window_pages != 0 && low_window_pages > kernel_low_window_pages) {
            printk("[VMM] low identity unmap clamped pages=0x");
            serial_write_hex64(low_window_pages);
            printk(" max=0x");
            serial_write_hex64(kernel_low_window_pages);
            printk("\r\n");
            low_window_pages = kernel_low_window_pages;
        }

        for (uintptr_t i = 0; i < low_window_pages; i++) {
            uintptr_t va = low_identity_unmap_start_va + (i * 4096);
            unmap_out unmap_md = vmm_unmap_page((uintptr_t *)k_root, va);

            if (!unmap_md.success) {
                printk("[VMM] low identity unmap failed at VA=0x");
                serial_write_hex64(va);
                printk(" err=0x");
                serial_write_hex64(unmap_md.error_code);
                printk("\r\n");
                break;
            }
        }
    } else {
        printk("[VMM] low identity unmap skipped: invalid range\r\n");
    }

    printk("[VMM] kmain higher-half VA=0x");
    serial_write_hex64(kmain_higher_half_va);
    printk("\r\n");

    uintptr_t after_transition_high_pa = 0;
    if (kmain_higher_half_va != 0) {
        after_transition_high_pa = vmm_query_page2(kmain_higher_half_va);
    }

    printk("[VMM] kmain higher-half VA=0x");
    serial_write_hex64(kmain_higher_half_va);
    printk(" post-transition PA(query2, high)=0x");
    serial_write_hex64(after_transition_high_pa);
    printk("\r\n");
    vmm_print_test_result("query kmain higher-half via vmm_query_page2", after_transition_high_pa != 0);

    if (before_transition_pa != 0 && after_transition_high_pa != 0) {
        vmm_print_test_result("kmain PA stable across transition", before_transition_pa == after_transition_high_pa);
    }

     idt_init();
    pic_init_for_keyboard();
    pit_init(100);
    __asm__ volatile("sti" ::: "memory");
    timer_ticks_boot = timer_ticks_get();
    printk("[INT] IDT loaded; PIC remapped; timer IRQ0 and keyboard IRQ1 active\r\n");
    printk("[SecureOS] kernel mb2_magic=0x");
    serial_write_hex32(mb2_magic);
    printk("\r\n");
    printk("[SecureOS] kernel mb2_info_ptr=0x");
    serial_write_hex32(mb2_info_ptr);
    printk("\r\n");


    if (fb_snapshot_valid) {
        uintptr_t fb_pa = (uintptr_t)fb_snapshot.common.framebuffer_addr;
        uintptr_t fb_va = phys_to_virt(fb_pa);
        uintptr_t fb_pa_after_transition = 0;
        fb_runtime = fb_snapshot;

        if (fb_va != 0) {
            fb_pa_after_transition = vmm_query_page2(fb_va);
            fb_runtime.common.framebuffer_addr = (uint64_t)fb_va;
        }

        printk("[SecureOS] framebuffer addr=0x");
        serial_write_hex64(fb_pa);
        printk(" phys_to_virt=0x");
        serial_write_hex64(fb_va);
        printk(" mapped_pa(va)=0x");
        serial_write_hex64(fb_pa_after_transition);
        printk("\r\n");

        if (fb_va != 0 && fb_pa_after_transition != 0) {
            fb_runtime_valid = 1;
            draw_boot_banner(&fb_runtime);
        } else {
            printk("[SecureOS] framebuffer unmapped after transition; skip draw\r\n");
        }
    } else {
        printk("[SecureOS] framebuffer tag missing (type 8)\r\n");
    }

    printk("[FAT32] initializing IDE-backed disk\r\n");
    if (block_device_init_ide_primary_master(&fat32_disk, "fat32-disk") != 0) {
        printk("[FAT32] disk init failed\r\n");
    } else if (fat32_mount(&mount, &fat32_disk, 0) != 0) {
        printk("[FAT32] mount failed\r\n");
    } else {
        syscall_fs_init(&mount);
        printk("[FAT32] disk ready: sectors=0x");
        serial_write_hex64(fat32_disk.sector_count);
        printk(" sector_size=0x");
        serial_write_hex32(fat32_disk.sector_size);
        printk("\r\n");

        fat32_dir_entry_view_t root_entry = {0};
        int root_resolve = fat32_path_resolve(&mount, "/", &root_entry);
        fat32_print_test_result("resolve /", root_resolve == 0 && root_entry.first_cluster == mount.root_cluster);

        printk("[FAT32] listing /\r\n");
        fat32_print_test_result("ls /", fat32_dir_list(&mount, "/", serial_print_dir_entry, 0) == 0);

        fat32_dir_entry_view_t readme_entry = {0};
        int readme_resolve = fat32_path_resolve(&mount, "/README.TXT", &readme_entry);
        fat32_print_test_result("resolve /README.TXT", readme_resolve == 0);

        if (readme_resolve == 0) {
            fat32_file_t readme_file = {0};
            uint8_t readme_buffer[128];
            uint32_t bytes_read = 0;
            int open_result = fat32_file_open(&mount, "/README.TXT", &readme_file);
            int read_result = -1;

            if (open_result == 0) {
                read_result = fat32_file_read(&readme_file, readme_buffer, sizeof(readme_buffer));
                if (read_result >= 0) {
                    bytes_read = (uint32_t)read_result;
                    read_result = 0;
                }
            }

            fat32_print_test_result("open /README.TXT", open_result == 0);
            fat32_print_test_result("read /README.TXT", read_result == 0 && bytes_read > 0);

            if (read_result == 0 && bytes_read > 0) {
                printk("[FAT32] /README.TXT bytes: ");
                serial_write_hex32(bytes_read);
                printk("\r\n");
                printk("[FAT32] /README.TXT text: ");
                for (uint32_t i = 0; i < bytes_read; i++) {
                    char c = (char)readme_buffer[i];
                    if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c <= 0x7E)) {
                        serial_write_char(c);
                    } else {
                        serial_write_char('.');
                    }
                }
                printk("\r\n");
                fat32_print_test_result("readme prefix", buffer_starts_with(readme_buffer, bytes_read, "This is SecureOS FAT32 storage."));
            }

            fat32_file_close(&readme_file);
        }

        fat32_dir_entry_view_t boot_dir_entry = {0};
        int boot_resolve = fat32_path_resolve(&mount, "/boot", &boot_dir_entry);
        fat32_print_test_result("resolve /boot", boot_resolve == 0 && (boot_dir_entry.attributes & FAT_ATTR_DIRECTORY));

        if (boot_resolve == 0) {
            printk("[FAT32] listing /boot\r\n");
            fat32_print_test_result("ls /boot", fat32_dir_list(&mount, "/boot", serial_print_dir_entry, 0) == 0);

            fat32_file_t pwned_file = {0};
            uint8_t pwned_buffer[64];
            uint32_t pwned_bytes = 0;
            int pwned_open = fat32_file_open(&mount, "/boot/PWNED.TXT", &pwned_file);
            int pwned_read = -1;

            if (pwned_open == 0) {
                pwned_read = fat32_file_read(&pwned_file, pwned_buffer, sizeof(pwned_buffer));
                if (pwned_read >= 0) {
                    pwned_bytes = (uint32_t)pwned_read;
                    pwned_read = 0;
                }
            }

            fat32_print_test_result("open /boot/PWNED.TXT", pwned_open == 0);
            fat32_print_test_result("read /boot/PWNED.TXT", pwned_read == 0 && pwned_bytes > 0);

            if (pwned_read == 0 && pwned_bytes > 0) {
                printk("[FAT32] /boot/PWNED.TXT text: ");
                for (uint32_t i = 0; i < pwned_bytes; i++) {
                    char c = (char)pwned_buffer[i];
                    if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c <= 0x7E)) {
                        serial_write_char(c);
                    } else {
                        serial_write_char('.');
                    }
                }
                printk("\r\n");
                fat32_print_test_result("pwned prefix", buffer_starts_with(pwned_buffer, pwned_bytes, "IT WORKS"));
            }

            fat32_file_close(&pwned_file);
        }

        {
            static const uint8_t smoke_a[] = "SMOKE-A\n";
            static const uint8_t smoke_b[] = "SMOKE-B\n";
            static const uint8_t smoke_ab[] = "SMOKE-A\nSMOKE-B\n";
            fat32_file_t smoke_file = {0};
            uint8_t smoke_read[64] = {0};
            int create_ok;
            int write_ok;
            int append_ok;
            int seek_ok;
            int read_ok;
            int truncate_ok;
            int delete_ok;
            int resolve_after_delete;

            (void)fat32_file_delete(&mount, "/SMOKE.TXT");

            create_ok = fat32_file_create(&mount, "/SMOKE.TXT", &smoke_file);
            fat32_print_test_result("smoke create", create_ok == 0);

            write_ok = (create_ok == 0)
                ? fat32_file_write(&smoke_file, smoke_a, (uint32_t)(sizeof(smoke_a) - 1))
                : -1;
            fat32_print_test_result("smoke write", write_ok == (int)(sizeof(smoke_a) - 1));

            append_ok = (write_ok >= 0)
                ? fat32_file_append(&smoke_file, smoke_b, (uint32_t)(sizeof(smoke_b) - 1))
                : -1;
            fat32_print_test_result("smoke append", append_ok == (int)(sizeof(smoke_b) - 1));

            seek_ok = (append_ok >= 0) ? fat32_file_seek(&smoke_file, 0) : -1;
            read_ok = (seek_ok == 0)
                ? fat32_file_read(&smoke_file, smoke_read, (uint32_t)(sizeof(smoke_ab) - 1))
                : -1;
            fat32_print_test_result("smoke readback", read_ok == (int)(sizeof(smoke_ab) - 1)
                && buffer_equals(smoke_read,
                                 (uint32_t)(sizeof(smoke_ab) - 1),
                                 smoke_ab,
                                 (uint32_t)(sizeof(smoke_ab) - 1)));

            truncate_ok = fat32_file_truncate(&smoke_file, (uint32_t)(sizeof(smoke_a) - 1));
            fat32_print_test_result("smoke truncate", truncate_ok == 0);

            fat32_file_close(&smoke_file);

            delete_ok = fat32_file_delete(&mount, "/SMOKE.TXT");
            fat32_print_test_result("smoke delete", delete_ok == 0);

            {
                fat32_dir_entry_view_t deleted_entry = {0};
                resolve_after_delete = fat32_path_resolve(&mount, "/SMOKE.TXT", &deleted_entry);
                fat32_print_test_result("smoke deleted not found", resolve_after_delete != 0);
            }
        }

        fat32_print_test_result("root cluster", mount.root_cluster >= 2);
        fat32_print_test_result("fsinfo signature", mount.fsinfo.lead_sig == 0x41615252u && mount.fsinfo.struct_sig == 0x61417272u);

        run_syscall_tests();
    }

    volatile char *vga = (volatile char *)phys_to_virt(0xB8000);
    if (vga != NULL) {
        for (int i = 0; i < 80 * 25 * 2; i += 2) {
            vga[i] = ' ';
            vga[i + 1] = 0x0F;
        }

        const char *msg = "KERNEL RUNNING!";
        for (int i = 0; msg[i] && i < 80; i++) {
            vga[i * 2] = msg[i];
            vga[i * 2 + 1] = 0x0F;
        }
    }

    // dump framebuffer info to serial
    printk("[FB] framebuffer info:\r\n");
    printk("[FB]  addr=%p\r\n", (void*)fb_snapshot.common.framebuffer_addr);
    printk("  width=%u  height=%u  bpp=%u\r\n",
        fb_snapshot.common.framebuffer_width,
        fb_snapshot.common.framebuffer_height,
        fb_snapshot.common.framebuffer_bpp);
    printk("[FB]  pitch=%u  type=%u\r\n",
        fb_snapshot.common.framebuffer_pitch,
        fb_snapshot.common.framebuffer_type);


    report_timer_irq_smoke_test(timer_ticks_boot, timer_ticks_get());

// Full pipeline test: create_proc -> elf_map_segments -> create_thread.
    // Buddy allocator's phys/virt bug (buddy.c) is fixed now (node_kva()
    // helper, phase-gated on physmap_ready) and the two companion bugs
    // (proc_kernel_root NULL, missing stack .perms) were fixed upstream -
    // this should map/build a real process+thread with no panic.

    // init heap before ELF load to use heap
    kheap_init();
    

    // printk("[ELF-LOAD] loading /RING3.EXE as a process (create_proc+map+create_thread)..\n");
    // elf_load_out load_md = elf_load_process(&mount, "/RING3.EXE");
    // if (load_md.success) {
    //     printk("[ELF-LOAD] test elf_load_process: PASS (pid=%d)\n", (int)load_md.p->id);
    // } else {
    //     printk("[ELF-LOAD] test elf_load_process: FAIL error_code=%d\n", (int)load_md.error_code);
    // }

    // init sched
    scheduler_init();

    // Ring3 preemption + idle-thread test. Registers the idle thread, loads
    // RING3.EXE and RING3P.EXE as two processes, and jumps into the first;
    // the timer does the rest. It never returns - restore_context() abandons
    // this stack - so it and the shell handoff below are mutually exclusive.
    // Build with -DRING3_PREEMPTION_TEST to get it back.
#ifdef RING3_PREEMPTION_TEST
    run_ring3_preemption_test(&mount);
#endif


    // Assaf DON'T PUT TEST CODE BELOW THIS POINT ----------------
    printk("[CONSOLE] Attempting to init console...\r\n");

    int console_ready = 0;

    if (!fb_runtime_valid) {
        printk("[CONSOLE] framebuffer runtime mapping not valid\r\n");
    } else if (console_init(&con, &fb_runtime) != 0) {
        printk("[CONSOLE] console init failed\r\n");
    } else {
        printk("[CONSOLE] console initialized\r\n");
        console_set_prompt(&con, "SSH> ");
        console_puts(&con, "-={ Welcome to the SecureOS! }=-\n\n\n");
        shell_init(&shell_ctx, &mount, &con);
        console_ready = 1;
    }

    if (!console_ready) {
        printk("[SHELL] no console; nothing to hand the CPU to\r\n");
        for (;;) { __asm__ volatile("hlt"); }
    }

    // The idle thread stays a plain hlt loop, separate from the shell. With
    // the shell on the ready queue, pick_next() only falls through to idle
    // when the shell is blocked on the keyboard and no program is running -
    // which is exactly when the CPU should be halted.
    if (install_idle_thread() == NULL || install_shell_thread() == NULL) {
        printk("[SHELL] thread setup failed; halting\r\n");
        for (;;) { __asm__ volatile("hlt"); }
    }

    // Hands kmain's stack over for good. The shell is the only thing on the
    // ready queue, so pick_next() returns it and restore_context() enters
    // shell_thread_entry() on its own kernel stack. Everything from here is
    // scheduled: the timer preempts between the shell and anything it loads,
    // and the keyboard IRQ wakes the shell out of its blocking read.
    // scheduler_pick_next() sets cpu->current_thread to the shell while this
    // code is still running on kmain's stack. A timer tick landing in that
    // window would save kmain's RIP and RSP into the shell's context and then
    // "resume" the shell there, so keep the two steps atomic. restore_context
    // clears IF itself as well; IRETQ brings it back from the shell's RFLAGS.
    __asm__ volatile("cli" ::: "memory");

    thread *first = scheduler_pick_next(0);
    if (first == NULL) {
        printk("[SHELL] pick_next found nothing to run\r\n");
        __asm__ volatile("sti" ::: "memory");
        for (;;) { __asm__ volatile("hlt"); }
    }

    restore_context(first->context);
}
