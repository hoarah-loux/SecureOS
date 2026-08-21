#pragma once

#include <stdint.h>
#include <stddef.h>

// Sentinel return from sys_getkey_wait meaning "no key buffered yet" -
// syscall_handler recognizes this and blocks the calling thread instead of
// returning it to userspace.
#define SYS_WOULD_BLOCK (-2)

// Returns 1 if scancode produced a buffered char, 0 otherwise (modifier
// key, key-release, or unmapped scancode).
int keyboard_handle_scancode(uint8_t scancode);
int keyboard_getchar_nonblock(char *out_char);
int keyboard_getline(char *buffer, size_t buffer_size);

// Matches `syscall_fn` from syscall.h: nonblocking single-key poll for
// DG_GetKey-style callers. Returns the next buffered char (0-255) or -1 if
// none is available. All six args are unused.
int64_t sys_getkey(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

// Blocking variant: returns the next buffered char (0-255), or
// SYS_WOULD_BLOCK if the buffer is empty (syscall_handler blocks the
// calling thread on the keyboard wait queue and retries on wake).
int64_t sys_getkey_wait(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);