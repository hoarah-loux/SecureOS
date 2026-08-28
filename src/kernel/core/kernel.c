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
#include <cpu_sse.h>
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
// into it: syscall_fs_init() keeps the mount, and shell_ctx_t keeps both the
// mount and the console. The framebuffer tag used to be here for the same
// reason; it now lives in framebuffer.c behind fb_set_runtime()/fb_get_runtime()
// so that the syscall layer can reach it too - see hdr/framebuffer.h.
static block_device_t fat32_disk;
static fat32_mount_t mount;
static console_t con;
static shell_ctx_t shell_ctx;

// Framebuffer snapshot shared with boot_stage_mark() below, so transition()
// (vmm.c) can stamp sub-stage markers - kmain's local copy is out of reach.
static struct multiboot_tag_framebuffer boot_fb_snapshot;
static int boot_fb_valid;

// 1 GiB page support (CPUID leaf 0x80000001, EDX bit 26) - the entry stub
// extends its identity map past 4 GiB with 1 GiB pages only when this is set.
static int cpu_has_1g_pages(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000001u));
    return (int)((edx >> 26) & 1u);
}

// Bare-metal boot-stage markers. On a machine with no serial port the only
// witness of a hang before the console exists is the screen itself, so each
// early boot phase stamps a small colored square in the top-left corner by
// writing the framebuffer directly. Which squares made it onto the glass is
// the diagnosis (photo the screen):
//   0 red    = kmain reached (framebuffer tag parsed)
//   1 yellow = pmm_init + bitmap tests survived
//   2 green  = vmm_init survived
//   3 blue   = transition() returned; higher-half live
// Pre-transition the framebuffer is reachable through its physical address
// (the entry stub identity-maps 0..4 GiB); post-transition through the
// physmap. `via_physmap` picks the path. 32 bpp only - real GOP firmware is
// 32 bpp in practice, and this must stay dumb enough to never fault; a
// framebuffer above 4 GiB (Resizable BAR) is skipped entirely, and the
// squares simply never appear - itself a diagnostic.
static void boot_marker(const struct multiboot_tag_framebuffer *fb,
                        int fb_valid, uint32_t index, uint32_t rgb,
                        int via_physmap) {
    enum { MARKER_SIZE = 32, MARKER_STRIDE = 40 };

    if (!fb_valid || fb->common.framebuffer_type != 1 ||
        fb->common.framebuffer_bpp != 32) {
        return;
    }

    uintptr_t base;
    if (via_physmap) {
        base = phys_to_virt((uintptr_t)fb->common.framebuffer_addr);
        if (base == 0) {
            return;
        }
    } else {
        // Pre-transition access goes through the entry stub's identity map:
        // 0..4 GiB always, 4..512 GiB only when the CPU has 1 GiB pages
        // (mirrors the stub's CPUID guard - a GPU BAR above 4 GiB is the
        // normal case with firmware "Above 4G Decoding" enabled).
        if (fb->common.framebuffer_addr >= 0x8000000000ULL) {
            return;
        }
        if (fb->common.framebuffer_addr >= 0x100000000ULL &&
            !cpu_has_1g_pages()) {
            return;
        }
        base = (uintptr_t)fb->common.framebuffer_addr;
    }

    uint32_t x0 = index * MARKER_STRIDE;
    if (x0 + MARKER_SIZE > fb->common.framebuffer_width ||
        MARKER_SIZE > fb->common.framebuffer_height) {
        return;
    }

    for (uint32_t y = 0; y < MARKER_SIZE; y++) {
        uint32_t *row = (uint32_t *)(void *)((uint8_t *)base +
                        (uint64_t)y * fb->common.framebuffer_pitch);
        for (uint32_t x = 0; x < MARKER_SIZE; x++) {
            row[x0 + x] = rgb;
        }
    }
}

// Exported wrapper so transition() (vmm.c) can stamp sub-stage markers into
// the same top-left square row: indices 4+ are transition sub-stages, drawn
// from the shared snapshot above. Declared in hdr/vmm.h.
void boot_stage_mark(uint32_t index, uint32_t rgb, int via_physmap) {
    boot_marker(&boot_fb_snapshot, boot_fb_valid, index, rgb, via_physmap);
}

// printk's framebuffer mirror (see hdr/serial.h). Registered the moment the
// console is up so that on a machine with no serial port - every modern
// desktop - boot progress and panics are still visible on screen. The shell
// writes to the same console; kernel messages after handoff interleave with
// it, which is exactly what a bare-metal debug session wants.
static void printk_console_mirror(char c) {
    console_putc(&con, c);
}

static inline uint8_t kmain_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

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

static uintptr_t read_cr3_root_phys(void) {
    uintptr_t cr3 = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3) :: "memory");
    return cr3 & ~((uintptr_t)0xFFF);
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

// No caller since the early framebuffer console replaced the banner (the
// console clear would wipe it the same instant); kept for reuse.
__attribute__((unused))
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

    // Run one command as if it had been typed, then fall through to the
    // interactive loop as usual.
    //
    // This exists so the ring3 test binaries can be driven headlessly. The
    // only way to start a program is to type "./NAME" at this prompt, and the
    // prompt is fed by the keyboard IRQ - so a QEMU run with -display none has
    // no way to launch anything, and every bring-up milestone would otherwise
    // need a human at a VM window. Build with e.g.
    //   make CFLAGS_EXTRA='-DSHELL_AUTORUN="\"./HELLO.EXE\""'
    // and the serial log tells the whole story on its own.
    //
    // shell_split_args() writes terminators into the line it is given, so the
    // literal is copied into the buffer rather than passed through.
#ifdef SHELL_AUTORUN
    {
        // Semicolons separate commands, so a single define can start more than
        // one program. That is not a convenience: the SSE test only means
        // anything with two vector-using processes on the ready queue at once,
        // since a lone one would pass even if the context switch saved no
        // vector state at all.
        const char *autorun = SHELL_AUTORUN;
        size_t at = 0;

        printk("[SHELL] autorun: %s\r\n", SHELL_AUTORUN);

        while (autorun[at] != '\0') {
            size_t n = 0;
            while (autorun[at] != '\0' && autorun[at] != ';' &&
                   n < sizeof(line_buffer) - 1) {
                line_buffer[n++] = autorun[at++];
            }
            line_buffer[n] = '\0';

            // Skip the separator, and any run of them.
            while (autorun[at] == ';') { at++; }

            if (n == 0) { continue; }

            // shell_split_args() writes terminators into the line it is given,
            // which is why each command is copied out rather than pointed at.
            char *auto_argv[16] = {0};
            shell_ctx.argc = shell_split_args(line_buffer, auto_argv, 16);
            shell_ctx.argv = auto_argv;
            (void)shell_eval(&shell_ctx);
        }

        line_buffer[0] = '\0';
    }
#endif

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

    // Input reverts here whenever a foreground program exits, so there is
    // never a moment where keystrokes have nowhere to go.
    keyboard_set_default_owner(shell_proc.proc->id);

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
    int fb_snapshot_valid = 0;
    int console_ready = 0;
    uint32_t ramdisk_mod_start = 0;
    uint32_t ramdisk_mod_end = 0;

    serial_init();

    // Framebuffer snapshot and the first boot marker come BEFORE the first
    // printk on purpose: on a machine with a broken/absent COM1 the very
    // first serial write is the first thing that can go wrong, and the red
    // square is the proof that kmain was reached at all.
    {
        const struct multiboot_tag_framebuffer *fb_pre =
            (const struct multiboot_tag_framebuffer *)mb2_find_tag(mb2_info_ptr, MULTIBOOT_TAG_TYPE_FRAMEBUFFER);
        if (fb_pre && fb_pre->common.size >= sizeof(struct multiboot_tag_framebuffer_common)) {
            fb_snapshot = *fb_pre;
            fb_snapshot_valid = 1;
            // Copy for boot_stage_mark(), so code outside kmain (transition's
            // sub-stage markers in vmm.c) can stamp progress on screen too.
            boot_fb_snapshot = fb_snapshot;
            boot_fb_valid = 1;
        }
    }

    boot_marker(&fb_snapshot, fb_snapshot_valid, 0, 0xFF0000, 0);

    printk("[SecureOS] kmain reached\r\n");

    // Snapshot the fat32-ramdisk module range while the Multiboot2 info is
    // still guaranteed mapped: after transition() the low-identity window is
    // partially unmapped, so the tag list must not be walked again later.
    // Only the physical range is kept; the data itself is read through the
    // physmap once it exists (pmm_init reserves the frames).
    {
        const struct multiboot_tag_module *mod_pre =
            (const struct multiboot_tag_module *)mb2_find_tag(mb2_info_ptr, MULTIBOOT_TAG_TYPE_MODULE);
        if (mod_pre && mod_pre->mod_end > mod_pre->mod_start) {
            ramdisk_mod_start = mod_pre->mod_start;
            ramdisk_mod_end = mod_pre->mod_end;
            printk("[SecureOS] boot module found: PA=0x%x..0x%x\r\n",
                   ramdisk_mod_start, ramdisk_mod_end);
        } else {
            printk("[SecureOS] no boot module (module tag missing)\r\n");
        }
    }

    printk("[VMM] initializing PMM/VMM\r\n");
    pmm_init(mb2_info_ptr, (uintptr_t)&_kernel_start, (uintptr_t)&_kernel_end);

    boot_marker(&fb_snapshot, fb_snapshot_valid, 1, 0xFFFF00, 0);

    uintptr_t *boot_root = (uintptr_t *)read_cr3_root_phys();
    uint64_t init_ok = vmm_init(boot_root);
    if (init_ok != 1) {
        printk("[VMM] vmm_init FAILED\r\n");
    }

    boot_marker(&fb_snapshot, fb_snapshot_valid, 2, 0x00FF00, 0);

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
        // transition() aliases the kernel window at KERNEL_IMAGE_BASE_VA +
        // link address (not +offset-from-window), so KIMG-style rebases
        // stay valid with the 16 MiB link base - mirror that here.
        kmain_higher_half_va = (uintptr_t)(KERNEL_IMAGE_BASE_VA + kmain_va);
    }

    if (kernel_low_window_end_va > kernel_low_window_start_va) {
        kernel_low_window_pages =
            (kernel_low_window_end_va - kernel_low_window_start_va) / 4096;
    }

    uintptr_t before_transition_pa = (uintptr_t)vmm_query_page(kmain_va);

    uint64_t transition_ok = transition(boot_root);
    if (transition_ok != 1) {
        printk("[VMM] transition FAILED\r\n");
    }

    // Only via the physmap when the transition actually happened - on an
    // early transition failure the old root is still active and the physmap
    // VA is unmapped, so drawing would fault with no IDT to catch it.
    if (transition_ok == 1) {
        boot_marker(&fb_snapshot, fb_snapshot_valid, 3, 0x0000FF, 1);
    }

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

    uintptr_t after_transition_high_pa = 0;
    if (kmain_higher_half_va != 0) {
        after_transition_high_pa = vmm_query_page2(kmain_higher_half_va);
    }

    if (before_transition_pa == 0 || after_transition_high_pa == 0 ||
        before_transition_pa != after_transition_high_pa) {
        printk("[VMM] kmain higher-half alias check FAILED\r\n");
    }

    // Before the IDT, and well before anything is scheduled: restore_context
    // uses FXRSTOR, which raises #UD while CR4.OSFXSR is clear, so this has to
    // be true for the very first context switch and not just for the first
    // user program. See src/arch/x86_64/cpu_sse.c.
    if (cpu_enable_sse()) {
        printk("[CPU] SSE enabled (CR0.EM=0, CR4.OSFXSR|OSXMMEXCPT=1)\r\n");
    } else {
        printk("[CPU] SSE unavailable; userland C code will not run\r\n");
    }

     idt_init();
    i8042_init();
    pic_init_for_keyboard();
    pit_init(100);
    __asm__ volatile("sti" ::: "memory");
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

        if (fb_va == 0) {
            // Framebuffer above the physmap clamp - a GPU BAR above 4 GiB
            // ("Above 4G Decoding"). The physmap only covers managed RAM, so
            // map the framebuffer MMIO into the same alias explicitly;
            // without this the machine boots blind.
            uint64_t fb_bytes = (uint64_t)fb_snapshot.common.framebuffer_pitch *
                                fb_snapshot.common.framebuffer_height;
            fb_va = vmm_map_high_physmap_range(fb_pa, fb_bytes);
            printk("[SecureOS] framebuffer above physmap clamp; explicit map %s\r\n",
                   fb_va != 0 ? "ok" : "FAILED");
        }

        // Local: fb_set_runtime() copies it, and nothing after this point reads
        // the tag through anything but fb_get_runtime().
        struct multiboot_tag_framebuffer fb_relocated = fb_snapshot;

        if (fb_va != 0) {
            fb_pa_after_transition = vmm_query_page2(fb_va);
            fb_relocated.common.framebuffer_addr = (uint64_t)fb_va;
        }

        printk("[SecureOS] framebuffer addr=0x");
        serial_write_hex64(fb_pa);
        printk(" phys_to_virt=0x");
        serial_write_hex64(fb_va);
        printk(" mapped_pa(va)=0x");
        serial_write_hex64(fb_pa_after_transition);
        printk("\r\n");

        if (fb_va != 0 && fb_pa_after_transition != 0 &&
            fb_set_runtime(&fb_relocated) == 0) {
            // Console comes up as early as the framebuffer allows, replacing
            // the old boot banner: from here on printk is mirrored on screen,
            // so a bare-metal boot that dies before the shell still shows
            // where. Serial keeps receiving everything regardless.
            if (console_init(&con, fb_get_runtime()) == 0) {
                console_set_active(&con);
                printk_set_mirror(printk_console_mirror);
                console_ready = 1;
                printk("[CONSOLE] framebuffer console up (%ux%u bpp=%u); printk mirrored to screen\r\n",
                       fb_get_runtime()->common.framebuffer_width,
                       fb_get_runtime()->common.framebuffer_height,
                       fb_get_runtime()->common.framebuffer_bpp);
            } else {
                printk("[CONSOLE] console init failed\r\n");
            }
        } else {
            printk("[SecureOS] framebuffer unmapped after transition; skip draw\r\n");
        }
    } else {
        printk("[SecureOS] framebuffer tag missing (type 8)\r\n");
    }

    // Userland storage. Preferred: the fat32-ramdisk Multiboot2 module GRUB
    // loaded into RAM - works on any machine, no disk controller involved,
    // which is the whole bare-metal story (the desktop's SATA is AHCI-only
    // and the IDE PIO driver cannot see it). Fallback: the legacy QEMU IDE
    // disk, so the old run/run-uefi targets keep working unchanged.
    int storage_ready = 0;
    if (ramdisk_mod_end > ramdisk_mod_start) {
        uintptr_t ramdisk_va = phys_to_virt((uintptr_t)ramdisk_mod_start);
        uint64_t ramdisk_bytes = (uint64_t)ramdisk_mod_end - ramdisk_mod_start;

        if (ramdisk_va != 0 &&
            block_device_init_memory(&fat32_disk, "fat32-ramdisk",
                                     (void *)ramdisk_va, ramdisk_bytes, 512) == 0) {
            storage_ready = 1;
            printk("[FAT32] ramdisk ready: PA=0x%x VA=0x%llx bytes=0x%llx\r\n",
                   ramdisk_mod_start,
                   (unsigned long long)ramdisk_va,
                   (unsigned long long)ramdisk_bytes);
        } else {
            printk("[FAT32] ramdisk module unusable; trying IDE fallback\r\n");
        }
    }

    if (!storage_ready) {
        printk("[FAT32] initializing IDE-backed disk\r\n");
        if (block_device_init_ide_primary_master(&fat32_disk, "fat32-disk") == 0) {
            storage_ready = 1;
        }
    }

    if (!storage_ready) {
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

    // The console itself came up right after the framebuffer runtime mapping
    // (see the fb block above) so the whole boot log is on screen; here it
    // only gets its shell face. Userland's fd 1 and fd 2 already reach the
    // screen via console_set_active(). `con` is file-scope, so the pointer
    // stays valid after kmain's frame is gone.
    // One-line keyboard diagnostic on screen: 0xFF means no i8042 at all
    // (firmware USB legacy emulation off or gone - check the BIOS "Legacy
    // USB Support" setting, and prefer Enabled over Auto); anything else
    // means the controller responds and input arrives via IRQ1 or the
    // timer-tick poll fallback.
    printk("[KBD] status entry=0x%x exit=0x%x selftest=0x%x porttest=0x%x\r\n",
           i8042_diag.status_entry, i8042_diag.status_exit,
           i8042_diag.self_test, i8042_diag.port_test);
    printk("[KBD] config before=0x%x after=0x%x ident=0x%x enable_ack=0x%x\r\n",
           i8042_diag.config_before, i8042_diag.config_after,
           i8042_diag.ident_first, i8042_diag.enable_ack);
    printk("[KBD] 0xFF in a response field = no answer within the bounded wait\r\n");

    if (console_ready) {
        console_set_prompt(&con, "SSH> ");
        console_puts(&con, "-={ Welcome to the SecureOS! }=-\n\n\n");
        shell_init(&shell_ctx, &mount, &con);
    } else {
        printk("[CONSOLE] no usable framebuffer console\r\n");
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
