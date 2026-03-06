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

#include "core/serial.h"

/* ---------------- Multiboot2 tag parsing (minimal) ---------------- */

struct mb2_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

struct mb2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
    /* followed by color info depending on framebuffer_type */
} __attribute__((packed));

struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    /* followed by entries */
} __attribute__((packed));

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed));

static const struct mb2_tag *mb2_find_tag(uint32_t mb2_info_ptr, uint32_t want_type) {
    const uint8_t *base = (const uint8_t *)(uintptr_t)mb2_info_ptr;
    if (!base) {
        return 0;
    }
    uint32_t total_size = *(const uint32_t *)(const void *)base;
    if (total_size < 8) {
        return 0;
    }
    const uint8_t *end = base + total_size;
    if (end < base) {
        return 0;
    }
    const uint8_t *p = base + 8; /* skip total_size + reserved */

    while (p + sizeof(struct mb2_tag) <= end) {
        const struct mb2_tag *tag = (const struct mb2_tag *)(const void *)p;
        if (tag->type == 0) {
            break;
        }
        if (tag->type == want_type) {
            return tag;
        }
        /* tags are 8-byte aligned */
        if (tag->size < sizeof(struct mb2_tag)) {
            break;
        }
        uint32_t next = (tag->size + 7u) & ~7u;
        if (next == 0 || p + next > end) {
            break;
        }
        p += next;
    }
    return 0;
}

/* ---------------- Framebuffer text (tiny built-in font) ---------------- */

static void fb_putpixel(const struct mb2_tag_framebuffer *fb, uint32_t x, uint32_t y, uint32_t rgb) {
    if (!fb || x >= fb->framebuffer_width || y >= fb->framebuffer_height) {
        return;
    }
    uint8_t *base = (uint8_t *)(uintptr_t)fb->framebuffer_addr;
    uint8_t *row = base + (uint64_t)y * fb->framebuffer_pitch;

    if (fb->framebuffer_bpp == 32) {
        ((uint32_t *)(void *)row)[x] = rgb;
    } else if (fb->framebuffer_bpp == 24) {
        uint8_t *px = row + x * 3;
        px[0] = (uint8_t)(rgb & 0xFF);
        px[1] = (uint8_t)((rgb >> 8) & 0xFF);
        px[2] = (uint8_t)((rgb >> 16) & 0xFF);
    } else if (fb->framebuffer_bpp == 16) {
        /* 5:6:5 */
        uint16_t r = (uint16_t)((rgb >> 19) & 0x1F);
        uint16_t g = (uint16_t)((rgb >> 10) & 0x3F);
        uint16_t b = (uint16_t)((rgb >> 3) & 0x1F);
        uint16_t v = (uint16_t)((r << 11) | (g << 5) | b);
        ((uint16_t *)(void *)row)[x] = v;
    }
}

static const uint8_t *font8x8_glyph(char c) {
    /* Minimal 8x8 uppercase glyphs needed for "KERNEL RUNNING!" */
    static const uint8_t SPACE[8] = {0,0,0,0,0,0,0,0};
    static const uint8_t EXCL[8]  = {0x20,0x20,0x20,0x20,0x00,0x20,0x00,0x00};
    static const uint8_t K_[8]    = {0x88,0x90,0xA0,0xC0,0xA0,0x90,0x88,0x00};
    static const uint8_t E_[8]    = {0xF8,0x80,0xF0,0x80,0x80,0xF8,0x00,0x00};
    static const uint8_t R_[8]    = {0xF0,0x88,0x88,0xF0,0xA0,0x90,0x88,0x00};
    static const uint8_t N_[8]    = {0x88,0xC8,0xA8,0x98,0x88,0x88,0x00,0x00};
    static const uint8_t L_[8]    = {0x80,0x80,0x80,0x80,0x80,0xF8,0x00,0x00};
    static const uint8_t U_[8]    = {0x88,0x88,0x88,0x88,0x88,0x70,0x00,0x00};
    static const uint8_t I_[8]    = {0xF8,0x20,0x20,0x20,0x20,0xF8,0x00,0x00};
    static const uint8_t G_[8]    = {0x78,0x80,0x80,0x98,0x88,0x78,0x00,0x00};

    switch (c) {
        case ' ': return SPACE;
        case '!': return EXCL;
        case 'K': return K_;
        case 'E': return E_;
        case 'R': return R_;
        case 'N': return N_;
        case 'L': return L_;
        case 'U': return U_;
        case 'I': return I_;
        case 'G': return G_;
        default:  return SPACE;
    }
}

static void fb_draw_char(const struct mb2_tag_framebuffer *fb, uint32_t x, uint32_t y, char c, uint32_t rgb) {
    const uint8_t *g = font8x8_glyph(c);
    for (uint32_t row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (uint32_t col = 0; col < 8; col++) {
            if (bits & (uint8_t)(0x80u >> col)) {
                fb_putpixel(fb, x + col, y + row, rgb);
            }
        }
    }
}

static void fb_draw_string(const struct mb2_tag_framebuffer *fb, uint32_t x, uint32_t y, const char *s, uint32_t rgb) {
    uint32_t cx = x;
    while (*s) {
        if (*s == '\n') {
            cx = x;
            y += 10;
            s++;
            continue;
        }
        fb_draw_char(fb, cx, y, *s++, rgb);
        cx += 8;
    }
}

/*
 * Main kernel entry point.
 * Called from src/boot/grub/entry64_mb2.asm as:
 *   kernel_main(magic, info_ptr)
 * with SysV AMD64 ABI args in RDI/RSI.
 */
void kernel_main(uint32_t mb2_magic, uint32_t mb2_info_ptr) {
    serial_init();
    serial_write_string("[SecureOS] kernel_main reached\r\n");
    serial_write_string("[SecureOS] kernel mb2_magic=0x");
    serial_write_hex32(mb2_magic);
    serial_write_string("\r\n");
    serial_write_string("[SecureOS] kernel mb2_info_ptr=0x");
    serial_write_hex32(mb2_info_ptr);
    serial_write_string("\r\n");

    /* Draw to the Multiboot2 framebuffer (this is what shows in the QEMU window under UEFI). */
    const struct mb2_tag_framebuffer *fb = (const struct mb2_tag_framebuffer *)mb2_find_tag(mb2_info_ptr, 8);
    if (fb) {
        if (fb->size < sizeof(*fb)) {
            serial_write_string("[SecureOS] framebuffer tag too small\r\n");
            fb = 0;
        }
    }

    /* Validate Multiboot2 E820 memory map (tag type 6) with a few sample entries. */
    const struct mb2_tag_mmap *mmap = (const struct mb2_tag_mmap *)mb2_find_tag(mb2_info_ptr, 6);
    if (mmap && mmap->size >= sizeof(*mmap) && mmap->entry_size >= sizeof(struct mb2_mmap_entry)) {
        const uint8_t *p = (const uint8_t *)(const void *)mmap + sizeof(*mmap);
        const uint8_t *end = (const uint8_t *)(const void *)mmap + mmap->size;

        serial_write_string("[SecureOS] mb2 mmap: entry_size=0x");
        serial_write_hex32(mmap->entry_size);
        serial_write_string(" entry_version=0x");
        serial_write_hex32(mmap->entry_version);
        serial_write_string("\r\n");

        for (int i = 0; i < 6 && p + mmap->entry_size <= end; i++) {
            const struct mb2_mmap_entry *e = (const struct mb2_mmap_entry *)(const void *)p;
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
        serial_write_hex32((uint32_t)(fb->framebuffer_addr & 0xFFFFFFFFu));
        serial_write_string("\r\n");
        /* type 1 = RGB; other types (indexed) need a palette. */
        if (fb->framebuffer_type == 1 && (fb->framebuffer_bpp == 32 || fb->framebuffer_bpp == 24 || fb->framebuffer_bpp == 16)) {
            fb_draw_string(fb, 16, 16, "KERNEL RUNNING!", 0x00FFFFFFu);
        } else {
            serial_write_string("[SecureOS] framebuffer format unsupported\r\n");
        }
    } else {
        serial_write_string("[SecureOS] framebuffer tag missing (type 8)\r\n");
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
