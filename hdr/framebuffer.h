#pragma once

#include <stdint.h>
#include <multiboot2.h>

void fb_draw_string(const struct multiboot_tag_framebuffer *fb, uint32_t x, uint32_t y, const char *s, uint32_t rgb);
