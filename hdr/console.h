#pragma once

#include <multiboot2.h>
#include <framebuffer.h>

typedef struct {
    unsigned int row;
    unsigned int col;
} console_cursor_t;

typedef struct {
    struct multiboot_tag_framebuffer* framebuffer;
    console_cursor_t cursor;
    unsigned int rows;
    unsigned int cols;
    char prompt[32];
    unsigned int fg_color;
    unsigned int bg_color;
} console_t;

int console_init(console_t* console, struct multiboot_tag_framebuffer* fb);
int console_putc(console_t* console, char c);
int console_puts(console_t* console, const char* str);
int console_printf(console_t* console, const char* fmt, ...);
void console_newline(console_t* console);
void console_backspace(console_t* console);
void console_set_prompt(console_t* console, const char* prompt);
int console_get_cursor_position(console_t* console, int* row, int* col);
void console_set_cursor_position(console_t* console, int row, int col);
int console_is_fb_supported(struct multiboot_tag_framebuffer* fb);