/*
 * kernel.c
 * Minimal kernel entry point for testing GRUB + Multiboot2 boot.
 *
 * Under UEFI GRUB you may see:
 *   "WARNING: no console will be available to OS"
 * which means VGA/text output is not a reliable debug channel.
 *
 * So: print a clear "kernel running" banner to COM1 serial.
 */

#include <stdint.h>

#include "multiboot2.h"
#include "core/serial.h"
#include "core/framebuffer.h"
#include "core/multiboot2_utils.h"
#include "fs/block_device.h"
#include "fs/fat32_fs.h"
#include "../stdlib/hexdump.h"
#include "../fs/tar.h"

static int serial_print_dir_entry(const fat32_dir_entry_view_t *entry, void *context) {
    (void)context;

    serial_write_string("[FAT32] dir: ");
    serial_write_string(entry->name);
    serial_write_string(" cluster=0x");
    serial_write_hex32(entry->first_cluster);
    serial_write_string(" size=0x");
    serial_write_hex32(entry->file_size);
    serial_write_string(" attr=0x");
    serial_write_hex32(entry->attributes);
    serial_write_string("\r\n");
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

static void fat32_print_test_result(const char *name, int passed) {
    serial_write_string("[FAT32] test ");
    serial_write_string(name);
    serial_write_string(passed ? ": PASS\r\n" : ": FAIL\r\n");
}

static void mb2_dump_tags(uint32_t mb2_info_ptr) {
    const uint8_t *base = (const uint8_t *)(uintptr_t)mb2_info_ptr;
    if (!base) {
        serial_write_string("[SecureOS] mb2 tags: info_ptr is null\r\n");
        return;
    }

    uint32_t total_size = *(const uint32_t *)(const void *)base;
    if (total_size < 8) {
        serial_write_string("[SecureOS] mb2 tags: total_size invalid\r\n");
        return;
    }

    const uint8_t *end = base + total_size;
    if (end < base) {
        serial_write_string("[SecureOS] mb2 tags: total_size overflow\r\n");
        return;
    }

    serial_write_string("[SecureOS] mb2 tags dump (type:size):\r\n");

    const uint8_t *p = base + 8; /* skip total_size + reserved */
    while (p + sizeof(struct multiboot_tag) <= end) {
        const struct multiboot_tag *tag = (const struct multiboot_tag *)(const void *)p;
        if (tag->type == 0) {
            serial_write_string("[SecureOS] tag=0 size=8 (end)\r\n");
            break;
        }

        serial_write_string("[SecureOS] tag=");
        serial_write_hex32(tag->type);
        serial_write_string(" size=");
        serial_write_hex32(tag->size);
        serial_write_string("\r\n");

        if (tag->size < sizeof(struct multiboot_tag)) {
            serial_write_string("[SecureOS] mb2 tags: invalid tag size\r\n");
            break;
        }

        uint32_t next = (tag->size + 7u) & ~7u;
        if (next == 0 || p + next > end) {
            serial_write_string("[SecureOS] mb2 tags: tag overrun\r\n");
            break;
        }
        p += next;
    }
}

static int serial_cstr_equal_suffix(const char *text, const char *suffix) {
    uint32_t text_len = 0;
    uint32_t suffix_len = 0;

    if (!text || !suffix) {
        return 0;
    }

    while (text[text_len] != '\0') {
        text_len++;
    }

    while (suffix[suffix_len] != '\0') {
        suffix_len++;
    }

    if (suffix_len > text_len) {
        return 0;
    }

    for (uint32_t i = 0; i < suffix_len; i++) {
        if (text[text_len - suffix_len + i] != suffix[i]) {
            return 0;
        }
    }

    return 1;
}

static const struct multiboot_tag_module *mb2_find_module_named(uint32_t mb2_info_ptr, const char *needle) {
    const uint8_t *base = (const uint8_t *)(uintptr_t)mb2_info_ptr;

    if (!base || !needle) {
        return 0;
    }

    uint32_t total_size = *(const uint32_t *)(const void *)base;
    if (total_size < 8) {
        return 0;
    }

    const uint8_t *end = base + total_size;
    const uint8_t *p = base + 8;

    while (p + sizeof(struct multiboot_tag) <= end) {
        const struct multiboot_tag *tag = (const struct multiboot_tag *)(const void *)p;
        if (tag->type == 0) {
            break;
        }

        if (tag->type == MULTIBOOT_TAG_TYPE_MODULE && tag->size >= sizeof(struct multiboot_tag_module)) {
            const struct multiboot_tag_module *module = (const struct multiboot_tag_module *)(const void *)p;
            const char *cmdline = module->cmdline;

            while (cmdline && *cmdline == ' ') {
                cmdline++;
            }

            if (cmdline) {
                const char *a = cmdline;
                const char *b = needle;
                while (*a && *b && *a == *b) {
                    a++;
                    b++;
                }

                if (*b == '\0' && (*a == '\0' || *a == ' ')) {
                    return module;
                }

                if (serial_cstr_equal_suffix(cmdline, needle)) {
                    return module;
                }
            }
        }

        uint32_t next = (tag->size + 7u) & ~7u;
        if (next == 0 || p + next > end) {
            break;
        }
        p += next;
    }

    return 0;
}

static void mb2_dump_modules(uint32_t mb2_info_ptr) {
    const uint8_t *base = (const uint8_t *)(uintptr_t)mb2_info_ptr;

    if (!base) {
        return;
    }

    uint32_t total_size = *(const uint32_t *)(const void *)base;
    if (total_size < 8) {
        return;
    }

    const uint8_t *end = base + total_size;
    const uint8_t *p = base + 8;
    uint32_t index = 0;

    while (p + sizeof(struct multiboot_tag) <= end) {
        const struct multiboot_tag *tag = (const struct multiboot_tag *)(const void *)p;
        if (tag->type == 0) {
            break;
        }

        if (tag->type == MULTIBOOT_TAG_TYPE_MODULE && tag->size >= sizeof(struct multiboot_tag_module)) {
            const struct multiboot_tag_module *module = (const struct multiboot_tag_module *)(const void *)p;
            serial_write_string("[SecureOS] module[");
            serial_write_hex32(index);
            serial_write_string("] start=0x");
            serial_write_hex32(module->mod_start);
            serial_write_string(" end=0x");
            serial_write_hex32(module->mod_end);
            serial_write_string(" cmdline=");
            serial_write_string(module->cmdline);
            serial_write_string("\r\n");
            index++;
        }

        uint32_t next = (tag->size + 7u) & ~7u;
        if (next == 0 || p + next > end) {
            break;
        }
        p += next;
    }
}

/*
 * Main kernel entry point.
 * Called from src/boot/grub/entry64_mb2.asm as:
 *   kmain(magic, info_ptr)
 * with SysV AMD64 ABI args in RDI/RSI.
 */
void kmain(uint32_t mb2_magic, uint32_t mb2_info_ptr) {
    serial_init();
    serial_write_string("[SecureOS] kmain reached\r\n");
    serial_write_string("[SecureOS] kernel mb2_magic=0x");
    serial_write_hex32(mb2_magic);
    serial_write_string("\r\n");
    serial_write_string("[SecureOS] kernel mb2_info_ptr=0x");
    serial_write_hex32(mb2_info_ptr);
    serial_write_string("\r\n");

    mb2_dump_tags(mb2_info_ptr);
    mb2_dump_modules(mb2_info_ptr);

    /* Draw to the Multiboot2 framebuffer (this is what shows in the QEMU window under UEFI). */
    const struct multiboot_tag_framebuffer *fb = (const struct multiboot_tag_framebuffer *)mb2_find_tag(mb2_info_ptr, MULTIBOOT_TAG_TYPE_FRAMEBUFFER);
    if (fb) {
        if (fb->common.size < sizeof(struct multiboot_tag_framebuffer_common)) {
            serial_write_string("[SecureOS] framebuffer tag too small\r\n");
            fb = 0;
        }
    }

    /* Validate Multiboot2 E820 memory map (tag type 6) with a few sample entries. */
    const struct multiboot_tag_mmap *mmap = (const struct multiboot_tag_mmap *)mb2_find_tag(mb2_info_ptr, MULTIBOOT_TAG_TYPE_MMAP);
    if (mmap && mmap->size >= sizeof(*mmap) && mmap->entry_size >= sizeof(struct multiboot_mmap_entry)) {
        const uint8_t *p = (const uint8_t *)(const void *)mmap->entries;
        const uint8_t *end = (const uint8_t *)(const void *)mmap + mmap->size;

        serial_write_string("[SecureOS] mb2 mmap: entry_size=0x");
        serial_write_hex32(mmap->entry_size);
        serial_write_string(" entry_version=0x");
        serial_write_hex32(mmap->entry_version);
        serial_write_string("\r\n");

        for (int i = 0; i < 6 && p + mmap->entry_size <= end; i++) {
            const struct multiboot_mmap_entry *e = (const struct multiboot_mmap_entry *)(const void *)p;
            serial_write_string("[SecureOS] mmap[");
            serial_write_hex32((uint32_t)i);
            serial_write_string("]: base=0x");
            serial_write_hex64(e->addr);
            serial_write_string(" len=0x");
            serial_write_hex64(e->len);
            serial_write_string(" type=0x");
            serial_write_hex32(e->type);
            serial_write_string("\r\n");
            p += mmap->entry_size;
        }
    } else {
        serial_write_string("[SecureOS] mb2 mmap tag missing or invalid\r\n");
    }
    if (fb) {
        serial_write_string("[SecureOS] framebuffer addr=0x");
        serial_write_hex32((uint32_t)(fb->common.framebuffer_addr & 0xFFFFFFFFu));
        serial_write_string("\r\n");
        /* type 1 = RGB; other types (indexed) need a palette. */
        if (fb->common.framebuffer_type == 1 && (fb->common.framebuffer_bpp == 32 || fb->common.framebuffer_bpp == 24 || fb->common.framebuffer_bpp == 16)) {
            fb_draw_string(fb, 16, 16, "KERNEL RUNNING!", 0x00FFFFFFu);
        } else {
            serial_write_string("[SecureOS] framebuffer format unsupported\r\n");
        }
    } else {
        serial_write_string("[SecureOS] framebuffer tag missing (type 8)\r\n");
    }

    // LOAD THE INITRD MODULE AND PRINT IT TO SERIAL
    /*
    struct multiboot_tag_module *initrd = (struct multiboot_tag_module*)mb2_find_tag(mb2_info_ptr, MULTIBOOT_TAG_TYPE_MODULE);
    if (initrd) {
        serial_write_string("[SecureOS] initrd module: start=0x");
        serial_write_hex32(initrd->mod_start);
        serial_write_string(" end=0x");
        serial_write_hex32(initrd->mod_end);
        serial_write_string(" cmdline=");
        serial_write_string(initrd->cmdline);
        serial_write_string("\r\n");
    } else {
        serial_write_string("[SecureOS] initrd module tag missing (type 3)\r\n");
    }

    // attempt to read the bytes from the initrd->mod_start to initrd->mod_end and print them to serial
    if (initrd) {
        uint32_t size = initrd->mod_end - initrd->mod_start;
        serial_write_string("[SecureOS] initrd content (hex):\n");
        // hexdump the first 256 bytes of the initrd content to serial
        parse_headers(initrd->mod_start);
        tar_print_file(initrd->mod_start, "./pwned.txt");
        tar_print_file(initrd->mod_start, "./doesnotexist.txt");
    */

    struct multiboot_tag_module *fat_module = (struct multiboot_tag_module*)mb2_find_tag(mb2_info_ptr, MULTIBOOT_TAG_TYPE_MODULE);
    const struct multiboot_tag_module *named_fat_module = mb2_find_module_named(mb2_info_ptr, "/boot/secureos.fat32.img");
    if (named_fat_module) {
        fat_module = (struct multiboot_tag_module *)named_fat_module;
        serial_write_string("[SecureOS] selected FAT32 module by exact/suffix match\r\n");
    }
    if (fat_module) {
        serial_write_string("[SecureOS] first module cmdline match state: ");
        serial_write_string(serial_cstr_equal_suffix(fat_module->cmdline, "/boot/secureos.fat32.img") ? "suffix-match" : "no-suffix-match");
        serial_write_string("\r\n");
    }

    if (!fat_module) {
        serial_write_string("[SecureOS] FAT32 module tag missing (type 3)\r\n");
    } else {
        serial_write_string("[SecureOS] FAT32 module: start=0x");
        serial_write_hex32(fat_module->mod_start);
        serial_write_string(" end=0x");
        serial_write_hex32(fat_module->mod_end);
        serial_write_string(" cmdline=");
        serial_write_string(fat_module->cmdline);
        serial_write_string("\r\n");

        uint32_t size = fat_module->mod_end - fat_module->mod_start;

        block_device_t boot_module_device;
        if (block_device_init_memory(
                &boot_module_device,
                "boot-module",
                (const void *)(uintptr_t)fat_module->mod_start,
                (uint64_t)size,
                512) == 0) {
            uint8_t sector0[512];
            if (block_device_read(&boot_module_device, 0, 1, sector0) == 0) {
                uint32_t magic = (uint32_t)sector0[0]
                               | ((uint32_t)sector0[1] << 8)
                               | ((uint32_t)sector0[2] << 16)
                               | ((uint32_t)sector0[3] << 24);
                serial_write_string("[SecureOS] block device smoke test OK, sector0=0x");
                serial_write_hex32(magic);
                serial_write_string("\r\n");
            } else {
                serial_write_string("[SecureOS] block device read failed\r\n");
            }
        } else {
            serial_write_string("[SecureOS] block device init failed\r\n");
        }

        serial_write_string("[FAT32] running filesystem smoke tests\r\n");
        fat32_mount_t mount = {0};
        if (block_device_init_memory(
                &boot_module_device,
                "fat32-image",
                (const void *)(uintptr_t)fat_module->mod_start,
                (uint64_t)size,
                512) != 0) {
            serial_write_string("[FAT32] mount device init failed\r\n");
        } else if (fat32_mount(&mount, &boot_module_device, 0) != 0) {
            serial_write_string("[FAT32] mount failed\r\n");
        } else {
            fat32_dir_entry_view_t root_entry = {0};
            int root_resolve = fat32_path_resolve(&mount, "/", &root_entry);
            fat32_print_test_result("resolve /", root_resolve == 0 && root_entry.first_cluster == mount.root_cluster);

            serial_write_string("[FAT32] listing /\r\n");
            int ls_root = fat32_dir_list(&mount, "/", serial_print_dir_entry, 0);
            fat32_print_test_result("ls /", ls_root == 0);

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
                    serial_write_string("[FAT32] /README.TXT bytes: ");
                    serial_write_hex32(bytes_read);
                    serial_write_string("\r\n");
                    serial_write_string("[FAT32] /README.TXT text: ");
                    for (uint32_t i = 0; i < bytes_read; i++) {
                        char c = (char)readme_buffer[i];
                        if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c <= 0x7E)) {
                            serial_write_char(c);
                        } else {
                            serial_write_char('.');
                        }
                    }
                    serial_write_string("\r\n");
                    fat32_print_test_result("readme prefix", buffer_starts_with(readme_buffer, bytes_read, "This is SecureOS FAT32 storage."));
                }

                fat32_file_close(&readme_file);
            }

            fat32_dir_entry_view_t boot_dir_entry = {0};
            int boot_resolve = fat32_path_resolve(&mount, "/boot", &boot_dir_entry);
            fat32_print_test_result("resolve /boot", boot_resolve == 0 && (boot_dir_entry.attributes & FAT_ATTR_DIRECTORY));

            if (boot_resolve == 0) {
                serial_write_string("[FAT32] listing /boot\r\n");
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
                    serial_write_string("[FAT32] /boot/PWNED.TXT text: ");
                    for (uint32_t i = 0; i < pwned_bytes; i++) {
                        char c = (char)pwned_buffer[i];
                        if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c <= 0x7E)) {
                            serial_write_char(c);
                        } else {
                            serial_write_char('.');
                        }
                    }
                    serial_write_string("\r\n");
                    fat32_print_test_result("pwned prefix", buffer_starts_with(pwned_buffer, pwned_bytes, "IT WORKS"));
                }

                fat32_file_close(&pwned_file);
            }

            fat32_print_test_result("root cluster", mount.root_cluster >= 2);
            fat32_print_test_result("fsinfo signature", mount.fsinfo.lead_sig == 0x41615252u && mount.fsinfo.struct_sig == 0x61417272u);
        }
    }

    /* Optional VGA write (may not be visible under UEFI GRUB). */
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

    while (1) {
        __asm__ volatile("hlt");
    }
}
