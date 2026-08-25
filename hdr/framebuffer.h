#pragma once

#include <stdint.h>
#include <multiboot2.h>

int fb_is_supported(const struct multiboot_tag_framebuffer *fb);

// The framebuffer the kernel is actually using, after kmain has relocated the
// Multiboot2 tag's physical address into the physmap.
//
// This lives here rather than in kernel.c because two unrelated consumers need
// it and neither can reach a `static` in kmain's translation unit: the console
// keeps the tag for the life of the kernel, and sys_fb_present() has to find it
// from the syscall layer. kmain calls fb_set_runtime() once, after the physmap
// transition; everything else reads it back.
//
// fb_set_runtime() copies the tag, so the caller may pass a local. It returns 0
// when the tag describes a framebuffer this code can draw on (fb_is_supported)
// and -1 otherwise, in which case fb_get_runtime() keeps returning NULL.
int fb_set_runtime(const struct multiboot_tag_framebuffer *fb);

// NULL until fb_set_runtime() has accepted a tag.
const struct multiboot_tag_framebuffer *fb_get_runtime(void);

// Scale a w x h buffer of 32-bit pixels up by the largest integer factor that
// fits, centre it, and draw it. src is packed, row-major, no padding, and each
// pixel is 0x00RRGGBB - the same layout the rest of this file calls `rgb`.
//
// Integer scaling only: a 320x200 buffer on a 1024x768 screen is drawn at 3x
// (960x600) with black bars around it, never stretched to fill. Non-integer
// scaling needs interpolation, which costs far more per pixel than this whole
// function, and doom's art is designed for hard pixel edges anyway.
//
// Returns 0 on success, -1 if the framebuffer or the arguments are unusable.
int fb_blit_scaled(const struct multiboot_tag_framebuffer *fb,
                   const uint32_t *src, uint32_t w, uint32_t h);
void fb_fill_rect(const struct multiboot_tag_framebuffer *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
void fb_scroll_up(const struct multiboot_tag_framebuffer *fb, uint32_t pixel_rows, uint32_t clear_rgb);
void fb_draw_string(const struct multiboot_tag_framebuffer *fb, uint32_t x, uint32_t y, const char *s, uint32_t rgb);
