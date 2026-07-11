#pragma once

#include <stdint.h>
#include <stddef.h>

void keyboard_handle_scancode(uint8_t scancode);
int keyboard_getchar_nonblock(char *out_char);
int keyboard_getline(char *buffer, size_t buffer_size);