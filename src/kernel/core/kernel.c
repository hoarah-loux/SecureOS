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
#include "../stdlib/hexdump.h"

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

    /* Draw to the Multiboot2 framebuffer (this is what shows in the QEMU window under UEFI). */
    const struct multiboot_tag_framebuffer *fb = (const struct multiboot_tag_framebuffer *)mb2_find_tag(mb2_info_ptr, MULTIBOOT_TAG_TYPE_FRAMEBUFFER);
    if (fb) {
        if (fb->common.size < sizeof(*fb)) {
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
        hexdump((const void *)(uintptr_t)initrd->mod_start, size);
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
