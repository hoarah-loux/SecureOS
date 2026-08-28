#pragma once

#include <stdint.h>
#include <stddef.h>

// Sentinel return from sys_getkey_wait meaning "no key buffered yet" -
// syscall_handler recognizes this and blocks the calling thread instead of
// returning it to userspace.
#define SYS_WOULD_BLOCK (-2)

// Returns 1 if the scancode produced anything a reader could consume - a raw
// event, a cooked character, or both - and 0 if it was swallowed (the 0xE0
// prefix byte, or an unmapped key in cooked-only mode). The IRQ handler uses
// this to decide whether to wake the threads waiting on the keyboard.
int keyboard_handle_scancode(uint8_t scancode);

// Minimal i8042 controller init: drain stale output, enable port 1 with
// IRQ + set-1 translation, start device scanning. Bounded waits throughout;
// a machine with no controller (floating 0xFF status) falls straight
// through. Call once at boot, before interrupts are enabled - real firmware
// keyboard emulation can come up wedged (stale byte + timeout error) and
// stays silent until this runs.
void i8042_init(void);

// What i8042_init() observed, for the bring-up report kmain prints. A
// response field left at 0xFF means nothing answered within the bounded
// wait - the telling case on firmware that services ports 60h/64h but has
// no device behind them.
typedef struct {
    uint8_t status_entry;   // port 0x64 before any command
    uint8_t status_exit;    // port 0x64 after init
    uint8_t self_test;      // 0xAA response, 0x55 = controller alive
    uint8_t port_test;      // 0xAB response, 0x00 = port 1 line good
    uint8_t config_before;  // config byte as firmware left it
    uint8_t config_after;   // config byte we wrote
    uint8_t ident_first;    // 0xF2 first response, 0xFA = a device answers
    uint8_t enable_ack;     // 0xF4 response, 0xFA = scanning enabled
} i8042_diag_t;

extern i8042_diag_t i8042_diag;

// Bytes the timer-tick poll has consumed from port 0x60 since boot, and the
// last one seen. Distinguishes "no bytes ever arrive" from "bytes arrive but
// decode to nothing" without a serial port.
extern uint32_t i8042_poll_bytes;
extern uint8_t i8042_poll_last;

// ---------------- raw scancodes ----------------
//
// The cooked path throws away everything a game needs. It drops key releases
// outright, has no handling for the 0xE0 prefix, and collapses what is left
// through an ASCII table - so arrows, RCtrl and RAlt do not exist as far as a
// program is concerned, and nothing can tell "still held" from "let go". doom
// needs press and release for every key it binds.
//
// Raw events are a parallel stream, not a replacement: the same interrupt
// feeds both rings, so the shell keeps reading characters exactly as before
// while a raw client reads events. One event per key transition, encoded as:
//
//   bits 0-7   the set-1 scancode with the release bit stripped
//   bit 8      the key was introduced by an 0xE0 prefix (arrows, RCtrl,
//              RAlt, keypad Enter, ...)
//   bit 15     this is a release, not a press
//
// So 'A' pressed is 0x001E and released 0x801E; up-arrow pressed is 0x0148 and
// released 0x8148.
#define KBD_SC_EXTENDED 0x0100u
#define KBD_SC_RELEASED 0x8000u

// Raw mode, per the process that owns input.
//
// While it is on, the cooked ring is not fed at all. That is not an
// optimisation: without it, every keystroke a game consumes would also pile up
// in the shell's line buffer, and the moment the game exited the shell would
// execute whatever that spelled. Both rings are flushed and raw mode is
// cleared whenever input ownership moves, so a program that dies mid-game
// cannot leave the shell deaf or holding a line of garbage.
void keyboard_set_raw_mode(int enabled);
int keyboard_raw_mode(void);

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

// Nonblocking raw event poll, for DG_GetKey and anything else that needs key
// releases. Returns the next event (see KBD_SC_* above) or -1 when the raw
// ring is empty or the caller does not own input. All six args are unused.
int64_t sys_getscancode(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

// Turns raw mode on (a0 non-zero) or off. Returns 0, or -1 if the caller does
// not own input - a background process must not be able to blind the
// foreground one. Both rings are flushed on the way in and out, so a mode
// change never delivers keystrokes typed under the other mode.
int64_t sys_kbd_raw(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);