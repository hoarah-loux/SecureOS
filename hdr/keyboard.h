#pragma once

#include <stdint.h>
#include <stddef.h>

void keyboard_handle_scancode(uint8_t scancode);
int keyboard_getchar_nonblock(char *out_char);
int keyboard_getline(char *buffer, size_t buffer_size);

// Matches `syscall_fn` from syscall.h: nonblocking single-key poll for
// DG_GetKey-style callers. Returns the next buffered char (0-255) or -1 if
// none is available. All six args are unused.
int64_t sys_getkey(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);