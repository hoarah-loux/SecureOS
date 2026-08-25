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

// ---------------- input ownership ----------------
//
// One keyboard, one ring buffer, and more than one thread that wants to read
// it: the shell sits blocked in SYS_getkey_wait for its whole life, and any
// program it launches wants the same keystrokes. Without an owner, whichever
// thread the scheduler happened to wake first would take the character, which
// is not a race that can be tuned away - it is undefined by construction.
//
// So exactly one process owns keyboard input at a time. Everyone else's read
// blocks until they own it. Ownership is a PID rather than a thread, because
// what a person means by "this program is in the foreground" is the program,
// not one of its threads.
//
// The shell is the default owner and gets it back whenever the foreground
// process exits, so there is never a state where keystrokes go nowhere.

// Records which process input reverts to. Called once, when the shell exists.
// Also takes ownership immediately if nobody holds it yet.
void keyboard_set_default_owner(uint64_t pid);

// Hands input to pid. The shell calls this when it launches a program.
void keyboard_grant_input(uint64_t pid);

// If pid currently owns input, hand it back to the default owner; otherwise do
// nothing. Called from the thread exit path, so a program that dies - cleanly
// or otherwise - cannot take the keyboard with it.
void keyboard_release_input(uint64_t pid);

uint64_t keyboard_input_owner(void);

// Whether the process running on this CPU right now is the input owner.
// sys_read() needs this for fd 0, which is a second door onto the same ring
// buffer as SYS_getkey and has to be gated the same way.
int keyboard_caller_owns_input(void);
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