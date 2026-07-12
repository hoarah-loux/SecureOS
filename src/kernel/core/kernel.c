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
#include <pmm.h>
#include <vmm.h>
#include <shell.h>
#include <console.h>

extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];

extern uint64_t transition(uintptr_t *old_root);
extern uintptr_t vmm_query_page2(uintptr_t virtual_adress);
extern uintptr_t _pmm_free_frame(uintptr_t address_to_free);

#define KERNEL_IMAGE_BASE_VA 0xFFFFFFFF80000000ULL
#define TWO_MIB_PAGE_MASK (~((uintptr_t)0x1FFFFF))

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

/*
 * Main kernel entry point.
 * Called from src/boot/grub/entry64_mb2.asm as:
 *   kmain(magic, info_ptr)
 * with SysV AMD64 ABI args in RDI/RSI.
 */
void kmain(uint32_t mb2_magic, uint32_t mb2_info_ptr) {
    struct multiboot_tag_framebuffer fb_snapshot = {0};
    struct multiboot_tag_framebuffer fb_runtime = {0};
    int fb_snapshot_valid = 0;
    int fb_runtime_valid = 0;

    serial_init();
    printk("[SecureOS] kmain reached\r\n");
    idt_init();
    pic_init_for_keyboard();
    __asm__ volatile("sti" ::: "memory");
    printk("[INT] IDT loaded; PIC remapped; keyboard IRQ1 active\r\n");
    printk("[SecureOS] kernel mb2_magic=0x");
    serial_write_hex32(mb2_magic);
    printk("\r\n");
    printk("[SecureOS] kernel mb2_info_ptr=0x");
    serial_write_hex32(mb2_info_ptr);
    printk("\r\n");

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
    uintptr_t kmain_higher_half_va = 0;
    if (kmain_va >= kernel_low_window_start_va) {
        kmain_higher_half_va = (uintptr_t)(KERNEL_IMAGE_BASE_VA + (kmain_va - kernel_low_window_start_va));
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

    block_device_t fat32_disk = {0};
    fat32_mount_t mount = {0};

    printk("[FAT32] initializing IDE-backed disk\r\n");
    if (block_device_init_ide_primary_master(&fat32_disk, "fat32-disk") != 0) {
        printk("[FAT32] disk init failed\r\n");
    } else if (fat32_mount(&mount, &fat32_disk, 0) != 0) {
        printk("[FAT32] mount failed\r\n");
    } else {
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
    }

    volatile char *vga = (volatile char *)0xB8000;
    for (int i = 0; i < 80 * 25 * 2; i += 2) {
        vga[i] = ' ';
        vga[i + 1] = 0x0F;
    }

    const char *msg = "KERNEL RUNNING!";
    for (int i = 0; msg[i] && i < 80; i++) {
        vga[i * 2] = msg[i];
        vga[i * 2 + 1] = 0x0F;
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
    
    printk("[CONSOLE] Attempting to init console...\r\n");

    console_t con = {0};
    if (!fb_runtime_valid) {
        printk("[CONSOLE] framebuffer runtime mapping not valid\r\n");
    } else if (console_init(&con, &fb_runtime) != 0) {
        printk("[CONSOLE] console init failed\r\n");
    } else {
        printk("[CONSOLE] console initialized\r\n");
        console_set_prompt(&con, "SSH> ");

        console_puts(&con, "-={ Welcome to the SecureOS! }=-\n\n\n");
        shell_ctx_t shell_ctx = {0};
        shell_init(&shell_ctx, &mount, &con);
        console_puts(&con, con.prompt);
        printk("[KBD] debug keypress loop active\r\n");

        // Find some random spot in memory to save buffer to:
        char* line_buffer = (char*)0x800000;
        size_t buffer_size = 256;
        size_t index = 0;
        while (index < buffer_size-1) {
            char c;
            if (keyboard_getchar_nonblock(&c)) {
                if (c == '\r' || c == '\n') {
                    line_buffer[index] = '\0';
                    console_newline(&con);
                    char* argv[16] = {0};
                    size_t argc = shell_split_args(line_buffer, argv, 16);
                    shell_ctx.argc = argc;
                    shell_ctx.argv = argv;
                    int shell_result = shell_eval(&shell_ctx);
                    index = 0;
                    line_buffer[index] = '\0';
                    console_puts(&con, con.prompt);
                } else if (c == '\b') {
                    if (index > 0) {
                        console_backspace(&con);
                        line_buffer[index] = '\0';
                        index--;
                    }
                } else {
                    line_buffer[index++] = c;
                    console_putc(&con, c);
                }
            }
        }
    }
}
