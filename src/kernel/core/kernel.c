/*
 * kernel.c
 * Minimal kernel entry point for testing GRUB + Multiboot2 boot.
 */

#include <stdint.h>

#include "multiboot2.h"
#include "core/serial.h"
#include "core/framebuffer.h"
#include "core/multiboot2_utils.h"
#include "fs/block_device.h"
#include "fs/ide_pio.h"
#include "fs/fat32_fs.h"

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
    serial_init();
    serial_write_string("[SecureOS] kmain reached\r\n");
    serial_write_string("[SecureOS] kernel mb2_magic=0x");
    serial_write_hex32(mb2_magic);
    serial_write_string("\r\n");
    serial_write_string("[SecureOS] kernel mb2_info_ptr=0x");
    serial_write_hex32(mb2_info_ptr);
    serial_write_string("\r\n");

    const struct multiboot_tag_framebuffer *fb = (const struct multiboot_tag_framebuffer *)mb2_find_tag(mb2_info_ptr, MULTIBOOT_TAG_TYPE_FRAMEBUFFER);
    if (fb && fb->common.size < sizeof(struct multiboot_tag_framebuffer_common)) {
        serial_write_string("[SecureOS] framebuffer tag too small\r\n");
        fb = 0;
    }

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
        draw_boot_banner(fb);
    } else {
        serial_write_string("[SecureOS] framebuffer tag missing (type 8)\r\n");
    }

    block_device_t fat32_disk = {0};
    fat32_mount_t mount = {0};

    serial_write_string("[FAT32] initializing IDE-backed disk\r\n");
    if (block_device_init_ide_primary_master(&fat32_disk, "fat32-disk") != 0) {
        serial_write_string("[FAT32] disk init failed\r\n");
    } else if (fat32_mount(&mount, &fat32_disk, 0) != 0) {
        serial_write_string("[FAT32] mount failed\r\n");
    } else {
        serial_write_string("[FAT32] disk ready: sectors=0x");
        serial_write_hex64(fat32_disk.sector_count);
        serial_write_string(" sector_size=0x");
        serial_write_hex32(fat32_disk.sector_size);
        serial_write_string("\r\n");

        fat32_dir_entry_view_t root_entry = {0};
        int root_resolve = fat32_path_resolve(&mount, "/", &root_entry);
        fat32_print_test_result("resolve /", root_resolve == 0 && root_entry.first_cluster == mount.root_cluster);

        serial_write_string("[FAT32] listing /\r\n");
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