#pragma once

#include <stdint.h>
#include <multiboot2.h>

int fb_is_supported(const struct multiboot_tag_framebuffer *fb);
void fb_fill_rect(const struct multiboot_tag_framebuffer *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
void fb_scroll_up(const struct multiboot_tag_framebuffer *fb, uint32_t pixel_rows, uint32_t clear_rgb);
void fb_draw_string(const struct multiboot_tag_framebuffer *fb, uint32_t x, uint32_t y, const char *s, uint32_t rgb);
