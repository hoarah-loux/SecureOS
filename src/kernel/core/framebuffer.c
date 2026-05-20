#include "core/framebuffer.h"

static void fb_putpixel(const struct multiboot_tag_framebuffer *fb, uint32_t x, uint32_t y, uint32_t rgb) {
    if (!fb || x >= fb->common.framebuffer_width || y >= fb->common.framebuffer_height) {
        return;
    }
    uint8_t *base = (uint8_t *)(uintptr_t)fb->common.framebuffer_addr;
    uint8_t *row = base + (uint64_t)y * fb->common.framebuffer_pitch;

    if (fb->common.framebuffer_bpp == 32) {
        ((uint32_t *)(void *)row)[x] = rgb;
    } else if (fb->common.framebuffer_bpp == 24) {
        uint8_t *px = row + x * 3;
        px[0] = (uint8_t)(rgb & 0xFF);
        px[1] = (uint8_t)((rgb >> 8) & 0xFF);
        px[2] = (uint8_t)((rgb >> 16) & 0xFF);
    } else if (fb->common.framebuffer_bpp == 16) {
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

static void fb_draw_char(const struct multiboot_tag_framebuffer *fb, uint32_t x, uint32_t y, char c, uint32_t rgb) {
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

void fb_draw_string(const struct multiboot_tag_framebuffer *fb, uint32_t x, uint32_t y, const char *s, uint32_t rgb) {
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
