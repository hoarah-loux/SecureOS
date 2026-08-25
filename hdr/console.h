#pragma once

#include <multiboot2.h>
#include <framebuffer.h>

typedef struct {
    unsigned int row;
    unsigned int col;
} console_cursor_t;

typedef struct {
    const struct multiboot_tag_framebuffer* framebuffer;
    console_cursor_t cursor;
    unsigned int rows;
    unsigned int cols;
    char prompt[32];
    unsigned int fg_color;
    unsigned int bg_color;
} console_t;

int console_init(console_t* console, const struct multiboot_tag_framebuffer* fb);
int console_putc(console_t* console, char c);
int console_puts(console_t* console, const char* str);
int console_printf(console_t* console, const char* fmt, ...);
void console_newline(console_t* console);
void console_backspace(console_t* console);
void console_set_prompt(console_t* console, const char* prompt);
int console_get_cursor_position(console_t* console, int* row, int* col);
void console_set_cursor_position(console_t* console, int row, int col);
int console_is_fb_supported(const struct multiboot_tag_framebuffer* fb);

// The console that userland's stdout and stderr write to.
//
// Same reason fb_get_runtime() exists (hdr/framebuffer.h): the console object
// lives in kmain's translation unit as a `static`, and sys_write() - in
// src/kernel/fs/fs_syscalls.c - cannot see it. kmain registers the console
// once, after console_init() succeeds, and the syscall layer reads it back.
//
// NULL until something registers one, which is the normal state on a machine
// with no usable framebuffer. Callers must handle that: writes then go to the
// serial port alone.
void console_set_active(console_t* console);
console_t* console_get_active(void);