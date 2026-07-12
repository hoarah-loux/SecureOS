#include <console.h>

enum {
    CONSOLE_CHAR_WIDTH = 8,
    CONSOLE_CHAR_HEIGHT = 10,
    CONSOLE_TAB_WIDTH = 4
};

static void console_fill_rect(console_t *console, unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int color) {
    if (!console || !console->framebuffer) {
        return;
    }

    fb_fill_rect(console->framebuffer, x, y, w, h, color);
}

static void console_clear_cell(console_t *console, unsigned int row, unsigned int col) {
    unsigned int x = col * CONSOLE_CHAR_WIDTH;
    unsigned int y = row * CONSOLE_CHAR_HEIGHT;
    console_fill_rect(console, x, y, CONSOLE_CHAR_WIDTH, CONSOLE_CHAR_HEIGHT, console->bg_color);
}

static void console_scroll_up(console_t *console) {
    if (!console || !console->framebuffer) {
        return;
    }

    fb_scroll_up(console->framebuffer, CONSOLE_CHAR_HEIGHT, console->bg_color);
}

static void console_put_visible_char(console_t *console, char c) {
    char text[2] = { c, '\0' };
    unsigned int x = console->cursor.col * CONSOLE_CHAR_WIDTH;
    unsigned int y = console->cursor.row * CONSOLE_CHAR_HEIGHT;

    console_clear_cell(console, console->cursor.row, console->cursor.col);
    fb_draw_string(console->framebuffer, x, y, text, console->fg_color);
}

int console_is_fb_supported(struct multiboot_tag_framebuffer* fb) {
    return fb_is_supported(fb);
}

// Return 0 on success, -1 on error
int console_init(console_t* console, struct multiboot_tag_framebuffer* fb) {
    if (!console || !console_is_fb_supported(fb)) {
        return -1; // Return -1 on error
    }

    console->framebuffer = fb;
    console->cursor.row = 0;
    console->cursor.col = 0;
    console->rows = fb->common.framebuffer_height / CONSOLE_CHAR_HEIGHT;
    console->cols = fb->common.framebuffer_width / CONSOLE_CHAR_WIDTH;
    console->prompt[0] = '\0'; // Initialize prompt to empty string
    console->fg_color = 0x00FFFFFF; // Default color (white)
    console->bg_color = 0x00000000; // Default color (black)

    if (console->rows == 0 || console->cols == 0) {
        return -1;
    }

    console_fill_rect(console, 0, 0, fb->common.framebuffer_width, fb->common.framebuffer_height, console->bg_color);

    return 0; // Return 0 on success
}

int console_putc(console_t* console, char c) {
    if (!console || !console->framebuffer) {
        return -1;
    }

    if (c == '\n') {
        console_newline(console);
        return 0;
    }

    if (c == '\r') {
        console->cursor.col = 0;
        return 0;
    }

    if (c == '\b') {
        console_backspace(console);
        return 0;
    }

    if (c == '\t') {
        unsigned int spaces = CONSOLE_TAB_WIDTH - (console->cursor.col % CONSOLE_TAB_WIDTH);
        for (unsigned int i = 0; i < spaces; i++) {
            (void)console_putc(console, ' ');
        }
        return 0;
    }

    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) {
        c = '.';
    }

    if (console->cursor.col >= console->cols) {
        console_newline(console);
    }

    console_put_visible_char(console, c);
    console->cursor.col++;

    if (console->cursor.col >= console->cols) {
        console_newline(console);
    }

    return 0; // Return 0 on success
}

int console_puts(console_t* console, const char* str) {
    if (!console || !str) {
        return -1;
    }

    while (*str) {
        if (console_putc(console, *str++) != 0) {
            return -1;
        }
    }

    return 0; // Return 0 on success
}

void console_newline(console_t* console) {
    if (!console) {
        return;
    }

    console->cursor.col = 0;
    console->cursor.row++;

    if (console->cursor.row >= console->rows) {
        console_scroll_up(console);
        console->cursor.row = console->rows - 1;
    }
}

void console_backspace(console_t* console) {
    if (!console) {
        return;
    }

    if (console->cursor.col == 0) {
        if (console->cursor.row == 0) {
            return;
        }
        console->cursor.row--;
        console->cursor.col = console->cols - 1;
    } else {
        console->cursor.col--;
    }

    console_clear_cell(console, console->cursor.row, console->cursor.col);
}

void console_set_prompt(console_t* console, const char* prompt) {
    if (!console) {
        return;
    }

    if (!prompt) {
        console->prompt[0] = '\0';
        return;
    }

    unsigned int i = 0;
    while (i < (unsigned int)(sizeof(console->prompt) - 1) && prompt[i] != '\0') {
        console->prompt[i] = prompt[i];
        i++;
    }
    console->prompt[i] = '\0';
}

int console_get_cursor_position(console_t* console, int* row, int* col) {
    if (!console) {
        return -1;
    }

    if (row) {
        *row = (int)console->cursor.row;
    }

    if (col) {
        *col = (int)console->cursor.col;
    }

    return 0; // Return 0 on success
}

void console_set_cursor_position(console_t* console, int row, int col) {
    if (!console) {
        return;
    }

    if (row < 0) {
        row = 0;
    }
    if (col < 0) {
        col = 0;
    }

    if ((unsigned int)row >= console->rows) {
        row = (int)(console->rows - 1);
    }
    if ((unsigned int)col >= console->cols) {
        col = (int)(console->cols - 1);
    }

    console->cursor.row = (unsigned int)row;
    console->cursor.col = (unsigned int)col;
}