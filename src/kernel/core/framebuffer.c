#include <framebuffer.h>
#include <font.h>

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

int fb_is_supported(const struct multiboot_tag_framebuffer *fb) {
    if (!fb) {
        return 0;
    }

    if (fb->common.framebuffer_type != 1) {
        return 0;
    }

    if (fb->common.framebuffer_bpp != 16 &&
        fb->common.framebuffer_bpp != 24 &&
        fb->common.framebuffer_bpp != 32) {
        return 0;
    }

    return 1;
}

void fb_fill_rect(const struct multiboot_tag_framebuffer *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb) {
    if (!fb || w == 0 || h == 0) {
        return;
    }

    uint32_t max_x = fb->common.framebuffer_width;
    uint32_t max_y = fb->common.framebuffer_height;
    uint32_t x_end = x + w;
    uint32_t y_end = y + h;

    if (x >= max_x || y >= max_y) {
        return;
    }

    if (x_end > max_x) {
        x_end = max_x;
    }
    if (y_end > max_y) {
        y_end = max_y;
    }

    for (uint32_t py = y; py < y_end; py++) {
        for (uint32_t px = x; px < x_end; px++) {
            fb_putpixel(fb, px, py, rgb);
        }
    }
}

void fb_scroll_up(const struct multiboot_tag_framebuffer *fb, uint32_t pixel_rows, uint32_t clear_rgb) {
    if (!fb || pixel_rows == 0) {
        return;
    }

    uint32_t height = fb->common.framebuffer_height;
    uint32_t pitch = fb->common.framebuffer_pitch;
    uint8_t *base = (uint8_t *)(uintptr_t)fb->common.framebuffer_addr;

    if (pixel_rows >= height) {
        fb_fill_rect(fb, 0, 0, fb->common.framebuffer_width, height, clear_rgb);
        return;
    }

    uint32_t shift = pitch * pixel_rows;
    uint32_t move_bytes = pitch * (height - pixel_rows);
    for (uint32_t i = 0; i < move_bytes; i++) {
        base[i] = base[i + shift];
    }

    fb_fill_rect(fb,
                 0,
                 height - pixel_rows,
                 fb->common.framebuffer_width,
                 pixel_rows,
                 clear_rgb);
}

static void fb_draw_char(const struct multiboot_tag_framebuffer *fb, uint32_t x, uint32_t y, char c, uint32_t rgb) {
    const uint8_t *g = font8x8_glyph(c);
    const uint32_t font_width = FONT_BWIDTH * 8u;
    for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
        const uint8_t *row_bits = g + (row * FONT_BWIDTH);
        for (uint32_t col = 0; col < font_width; col++) {
            uint8_t bits = row_bits[col / 8u];
            if (bits & (uint8_t)(1u << (col % 8u))) {
                fb_putpixel(fb, x + col, y + row, rgb);
            }
        }
    }
}

void fb_draw_string(const struct multiboot_tag_framebuffer *fb, uint32_t x, uint32_t y, const char *s, uint32_t rgb) {
    const uint32_t font_width = FONT_BWIDTH * 8u;
    const uint32_t line_height = FONT_HEIGHT + 2u;
    uint32_t cx = x;
    while (*s) {
        if (*s == '\n') {
            cx = x;
            y += line_height;
            s++;
            continue;
        }
        fb_draw_char(fb, cx, y, *s++, rgb);
        cx += font_width;
    }
}
