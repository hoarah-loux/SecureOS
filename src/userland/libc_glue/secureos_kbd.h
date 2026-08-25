#pragma once

// Raw keyboard events, for programs that need more than characters.
//
// stdin gives you cooked text: printable characters, presses only, no arrows.
// That is right for a shell and useless for a game, which has to know that a
// key is still held and which of the two Ctrls it was. These read the parallel
// raw stream instead.
//
// An event is one key transition:
//
//   bits 0-7   the set-1 scancode, release bit stripped
//   bit 8      SECUREOS_KEY_EXTENDED - the key was introduced by an 0xE0
//              prefix: arrows, RCtrl, RAlt, keypad Enter, and friends
//   bit 15     SECUREOS_KEY_RELEASED - a release rather than a press
//
// 'A' pressed is 0x001E, released 0x801E. Up-arrow pressed is 0x0148.

#include <stdint.h>

#include "secureos_syscall.h"

#define SECUREOS_KEY_EXTENDED 0x0100u
#define SECUREOS_KEY_RELEASED 0x8000u

// Strip the flags to get the scancode, or test them.
#define SECUREOS_KEY_CODE(ev)        ((unsigned)(ev) & 0xFFu)
#define SECUREOS_KEY_IS_EXTENDED(ev) (((unsigned)(ev) & SECUREOS_KEY_EXTENDED) != 0)
#define SECUREOS_KEY_IS_RELEASE(ev)  (((unsigned)(ev) & SECUREOS_KEY_RELEASED) != 0)

// Turn raw mode on or off for this process.
//
// While it is on, keystrokes stop being translated into characters, so stdin
// goes quiet - that is the point. Without it every key the program consumed
// would also accumulate in the shell's line buffer and be executed as a
// command the moment the program exited.
//
// Only the foreground process may do this; anyone else gets -1. Both keyboard
// rings are flushed on the way in and out, so no keystroke crosses the
// boundary.
static inline int secureos_kbd_raw(int enable)
{
    return (int)__sys1(SYS_kbd_raw, enable);
}

// Next raw event, or -1 if none is buffered. Never blocks: this is a poll, to
// be called once per frame.
static inline int secureos_getscancode(void)
{
    return (int)__sys0(SYS_getscancode);
}
