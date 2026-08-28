#include <framebuffer.h>
#include <font.h>

// The framebuffer the kernel actually draws on.
//
// kmain owns the discovery of the Multiboot2 tag and the physmap address it
// has to be rewritten to, but not the storage: the console holds this pointer
// for the life of the kernel and sys_fb_present() has to reach it from the
// syscall layer, neither of which can see a `static` in kernel.c. kmain hands
// the finished tag over once, here, and reads it back through fb_get_runtime().
static struct multiboot_tag_framebuffer fb_runtime;
static int fb_runtime_valid;

// Where each color channel sits in a native pixel, taken from the Multiboot2
// tag's RGB field descriptors. QEMU's stdvga and most GOP firmware use
// xRGB8888 (red at 16, green at 8, blue at 0), but the Multiboot2 spec lets
// firmware report any layout, and drawing 0xRRGGBB raw on a machine that
// disagrees swaps red and blue on every pixel. Everything that writes a pixel
// goes through fb_pack_color() so the layout is honored in one place.
static struct {
    uint8_t r_pos, g_pos, b_pos;
    uint8_t r_size, g_size, b_size;
    int native_xrgb32;
} fb_format;

static void fb_format_from_tag(const struct multiboot_tag_framebuffer *fb) {
    // The RGB descriptor bytes sit right after the common header; only trust
    // them when the tag is big enough to contain them and the mask sizes are
    // sane. Otherwise assume the de-facto standard layout for the depth.
    int have_fields =
        fb->common.size >= sizeof(struct multiboot_tag_framebuffer_common) + 6 &&
        fb->framebuffer_red_mask_size != 0 &&
        fb->framebuffer_green_mask_size != 0 &&
        fb->framebuffer_blue_mask_size != 0;

    if (have_fields) {
        fb_format.r_pos = fb->framebuffer_red_field_position;
        fb_format.r_size = fb->framebuffer_red_mask_size;
        fb_format.g_pos = fb->framebuffer_green_field_position;
        fb_format.g_size = fb->framebuffer_green_mask_size;
        fb_format.b_pos = fb->framebuffer_blue_field_position;
        fb_format.b_size = fb->framebuffer_blue_mask_size;
    } else if (fb->common.framebuffer_bpp == 16) {
        fb_format.r_pos = 11; fb_format.r_size = 5;
        fb_format.g_pos = 5;  fb_format.g_size = 6;
        fb_format.b_pos = 0;  fb_format.b_size = 5;
    } else {
        fb_format.r_pos = 16; fb_format.r_size = 8;
        fb_format.g_pos = 8;  fb_format.g_size = 8;
        fb_format.b_pos = 0;  fb_format.b_size = 8;
    }

    fb_format.native_xrgb32 =
        fb->common.framebuffer_bpp == 32 &&
        fb_format.r_pos == 16 && fb_format.r_size == 8 &&
        fb_format.g_pos == 8 && fb_format.g_size == 8 &&
        fb_format.b_pos == 0 && fb_format.b_size == 8;
}

static inline uint32_t fb_pack_channel(uint32_t v8, uint8_t pos, uint8_t size) {
    if (size >= 8) {
        return (v8 << (size - 8)) << pos;
    }
    return (v8 >> (8 - size)) << pos;
}

// 0xRRGGBB (the color space every caller speaks) to a native pixel value.
static uint32_t fb_pack_color(uint32_t rgb) {
    if (fb_format.native_xrgb32) {
        return rgb;
    }
    return fb_pack_channel((rgb >> 16) & 0xFFu, fb_format.r_pos, fb_format.r_size) |
           fb_pack_channel((rgb >> 8) & 0xFFu, fb_format.g_pos, fb_format.g_size) |
           fb_pack_channel(rgb & 0xFFu, fb_format.b_pos, fb_format.b_size);
}

int fb_set_runtime(const struct multiboot_tag_framebuffer *fb) {
    if (!fb_is_supported(fb)) {
        return -1;
    }

    fb_runtime = *fb;
    fb_format_from_tag(&fb_runtime);
    fb_runtime_valid = 1;
    return 0;
}

const struct multiboot_tag_framebuffer *fb_get_runtime(void) {
    return fb_runtime_valid ? &fb_runtime : 0;
}

static void fb_putpixel(const struct multiboot_tag_framebuffer *fb, uint32_t x, uint32_t y, uint32_t rgb) {
    if (!fb || x >= fb->common.framebuffer_width || y >= fb->common.framebuffer_height) {
        return;
    }
    uint8_t *base = (uint8_t *)(uintptr_t)fb->common.framebuffer_addr;
    uint8_t *row = base + (uint64_t)y * fb->common.framebuffer_pitch;
    uint32_t px = fb_pack_color(rgb);

    if (fb->common.framebuffer_bpp == 32) {
        ((uint32_t *)(void *)row)[x] = px;
    } else if (fb->common.framebuffer_bpp == 24) {
        uint8_t *dst = row + x * 3;
        dst[0] = (uint8_t)(px & 0xFF);
        dst[1] = (uint8_t)((px >> 8) & 0xFF);
        dst[2] = (uint8_t)((px >> 16) & 0xFF);
    } else if (fb->common.framebuffer_bpp == 16) {
        ((uint16_t *)(void *)row)[x] = (uint16_t)px;
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

// ---------------- Scaled 32-bit blit ----------------
//
// This is the hot path of the whole graphics stack: doom presents 320x200 at
// 35 frames a second, and at 3x that is 576,000 destination pixels per frame.
// Everything below is shaped by the fact that the kernel builds at -O0, so
// nothing is optimised away and every line written here is a line executed.
//
// Two consequences worth stating, because they look like premature
// micro-optimisation and are not:
//
//   - fb_putpixel() is not used. It re-validates the coordinates and re-runs a
//     bits-per-pixel switch for every pixel; at -O0 that is roughly a dozen
//     instructions of overhead on a store that should be one.
//   - copies move uint32_t at a time, not bytes. hdr/string.h's memcpy is a
//     byte loop, which at -O0 turns a 2.3 MB frame into 2.3 million iterations
//     for the row duplication alone.
//
// Scaling exploits the structure of the problem rather than working harder:
// every group of s output rows is identical, so only the first is built pixel
// by pixel and the rest are copied from it. That makes the per-pixel expansion
// cost proportional to w*h*s rather than w*h*s*s.

// Copy n pixels. Named rather than inlined at the call sites so the intent -
// this moves whole pixels, never bytes - is visible where it matters.
static inline void fb_copy_px(uint32_t *dst, const uint32_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

// The generic path, for the 16 and 24 bpp modes. Correct, and slow enough that
// nothing should rely on it for animation; it exists so a machine that hands us
// an unusual mode still shows a picture instead of nothing.
static void fb_blit_scaled_generic(const struct multiboot_tag_framebuffer *fb,
                                   const uint32_t *src, uint32_t w, uint32_t h,
                                   uint32_t s, uint32_t x0, uint32_t y0) {
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *src_row = src + (uint64_t)y * w;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px = src_row[x];
            for (uint32_t dy = 0; dy < s; dy++) {
                for (uint32_t dx = 0; dx < s; dx++) {
                    fb_putpixel(fb, x0 + x * s + dx, y0 + y * s + dy, px);
                }
            }
        }
    }
}

int fb_blit_scaled(const struct multiboot_tag_framebuffer *fb,
                   const uint32_t *src, uint32_t w, uint32_t h) {
    if (!fb_is_supported(fb) || !src || w == 0 || h == 0) {
        return -1;
    }

    uint32_t fb_w   = fb->common.framebuffer_width;
    uint32_t fb_h   = fb->common.framebuffer_height;
    uint32_t pitch  = fb->common.framebuffer_pitch;

    if (w > fb_w || h > fb_h) {
        // No integer scale fits, and downscaling would mean dropping pixels.
        // Refuse rather than draw something misleading.
        return -1;
    }

    // Largest integer factor that fits in both directions, never below 1.
    uint32_t sx = fb_w / w;
    uint32_t sy = fb_h / h;
    uint32_t s  = (sx < sy) ? sx : sy;
    if (s == 0) {
        s = 1;
    }

    uint32_t dst_w = w * s;
    uint32_t dst_h = h * s;
    uint32_t x0    = (fb_w - dst_w) / 2u;
    uint32_t y0    = (fb_h - dst_h) / 2u;

    // Clear the letterbox, but only when the geometry changes.
    //
    // The bars never contain anything the caller drew, so they only need
    // painting once - but they do need painting at least once, or whatever the
    // console left on screen stays visible around the picture forever. Doing it
    // every frame would cost more than the blit itself on a large screen.
    static uint32_t last_x0, last_y0, last_w, last_h;
    static int      last_valid;
    if (!last_valid || last_x0 != x0 || last_y0 != y0 ||
        last_w != dst_w || last_h != dst_h) {
        fb_fill_rect(fb, 0, 0, fb_w, fb_h, 0x000000);
        last_x0 = x0;
        last_y0 = y0;
        last_w  = dst_w;
        last_h  = dst_h;
        last_valid = 1;
    }

    if (fb->common.framebuffer_bpp != 32) {
        fb_blit_scaled_generic(fb, src, w, h, s, x0, y0);
        return 0;
    }

    uint8_t *base = (uint8_t *)(uintptr_t)fb->common.framebuffer_addr;

    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *src_row = src + (uint64_t)y * w;
        uint8_t  *dst_row_bytes = base + (uint64_t)(y0 + y * s) * pitch;
        uint32_t *dst_row       = (uint32_t *)(void *)(dst_row_bytes + (uint64_t)x0 * 4u);

        // Build the first of this group's s identical rows. Packing happens
        // once per SOURCE pixel: the s-fold horizontal and vertical
        // duplication below copies already-native values.
        if (s == 1) {
            if (fb_format.native_xrgb32) {
                fb_copy_px(dst_row, src_row, w);
            } else {
                for (uint32_t x = 0; x < w; x++) {
                    dst_row[x] = fb_pack_color(src_row[x]);
                }
            }
        } else {
            uint32_t i = 0;
            for (uint32_t x = 0; x < w; x++) {
                uint32_t px = fb_pack_color(src_row[x]);
                for (uint32_t k = 0; k < s; k++) {
                    dst_row[i++] = px;
                }
            }
        }

        // Then duplicate it downwards. Reading back out of the framebuffer is
        // slower than reading from RAM would be, but the alternative is a
        // scratch buffer sized to the widest screen we might ever see, and the
        // copy is a straight run either way.
        for (uint32_t k = 1; k < s; k++) {
            uint32_t *rep = (uint32_t *)(void *)(dst_row_bytes + (uint64_t)k * pitch +
                                                 (uint64_t)x0 * 4u);
            fb_copy_px(rep, dst_row, dst_w);
        }
    }

    return 0;
}
