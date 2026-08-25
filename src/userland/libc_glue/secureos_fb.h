#pragma once

// The whole graphics API: hand the kernel a buffer of pixels, it puts them on
// the screen.
//
// There is no mapping, no double buffer to flip and nothing to open first. The
// framebuffer is mapped supervisor-only in the physmap and sys_mmap has no
// device-backed path, so the pixels are copied through the kernel on every
// call. That copy is the reason this is one function and not a device.

#include <stdint.h>

#include "secureos_syscall.h"

// Draw w*h pixels, scaled up by the largest integer factor that fits the
// screen, centred, with black bars around it.
//
// px points at w*h packed 32-bit pixels, row-major, no padding between rows,
// each 0x00RRGGBB. The buffer is read and not retained: it may be reused or
// freed as soon as this returns.
//
// Returns 0, or -1 if there is no usable framebuffer or the buffer does not lie
// entirely inside this process's address space.
static inline int secureos_fb_present(const uint32_t *px, uint32_t w, uint32_t h)
{
    return (int)__sys3(SYS_fb_present, px, w, h);
}
