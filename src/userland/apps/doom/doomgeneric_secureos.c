// doomgeneric backend for SecureOS.
//
// Four kernel facilities carry the whole port:
//
//   SYS_fb_present   the frame, scaled and centred by the kernel
//   SYS_getscancode  raw key transitions, presses *and* releases
//   SYS_gettime      milliseconds since boot, 10 ms resolution
//   SYS_sleep        the frame limiter
//
// There is no window, no event loop and nothing to open first, so DG_Init is
// one call and DG_DrawFrame is another.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "m_argv.h"

#include "secureos_fb.h"
#include "secureos_kbd.h"
#include "secureos_syscall.h"

// A raw event carries the set-1 scancode in bits 0-7 and the 0xE0 prefix in
// bit 8, so an extended key is looked up as 0x100 | code. Both halves are
// handled by one table indexed on that 9-bit value.
static unsigned char scancode_to_doomkey(unsigned code, int extended)
{
    if (extended) {
        switch (code) {
            case 0x48: return KEY_UPARROW;
            case 0x50: return KEY_DOWNARROW;
            case 0x4B: return KEY_LEFTARROW;
            case 0x4D: return KEY_RIGHTARROW;
            case 0x1D: return KEY_FIRE;        // right ctrl
            case 0x38: return KEY_RALT;        // right alt
            case 0x1C: return KEY_ENTER;       // keypad enter
            case 0x47: return KEY_HOME;
            case 0x4F: return KEY_END;
            case 0x49: return KEY_PGUP;
            case 0x51: return KEY_PGDN;
            case 0x52: return KEY_INS;
            case 0x53: return KEY_DEL;
            default:   return 0;
        }
    }

    switch (code) {
        case 0x01: return KEY_ESCAPE;
        case 0x1C: return KEY_ENTER;
        case 0x0F: return KEY_TAB;
        case 0x0E: return KEY_BACKSPACE;
        case 0x39: return KEY_USE;             // space
        case 0x1D: return KEY_FIRE;            // left ctrl
        case 0x38: return KEY_LALT;            // left alt
        case 0x2A: return KEY_RSHIFT;          // left shift
        case 0x36: return KEY_RSHIFT;          // right shift
        case 0x3A: return KEY_CAPSLOCK;
        case 0x45: return KEY_PAUSE;

        // Arrows again, unprefixed: a keyboard in numeric-keypad mode sends
        // these bare, and doom's own menus are driven by them.
        case 0x48: return KEY_UPARROW;
        case 0x50: return KEY_DOWNARROW;
        case 0x4B: return KEY_LEFTARROW;
        case 0x4D: return KEY_RIGHTARROW;

        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;

        case 0x0C: return KEY_MINUS;
        case 0x0D: return KEY_EQUALS;

        // Digits on the number row. 0x02..0x0B is '1'..'9' then '0'.
        case 0x02: return '1';
        case 0x03: return '2';
        case 0x04: return '3';
        case 0x05: return '4';
        case 0x06: return '5';
        case 0x07: return '6';
        case 0x08: return '7';
        case 0x09: return '8';
        case 0x0A: return '9';
        case 0x0B: return '0';

        // Letters, in scancode order rather than alphabetical - doom wants
        // lower case, which is what its menu and cheat code both compare
        // against.
        case 0x10: return 'q';
        case 0x11: return 'w';
        case 0x12: return 'e';
        case 0x13: return 'r';
        case 0x14: return 't';
        case 0x15: return 'y';
        case 0x16: return 'u';
        case 0x17: return 'i';
        case 0x18: return 'o';
        case 0x19: return 'p';
        case 0x1E: return 'a';
        case 0x1F: return 's';
        case 0x20: return 'd';
        case 0x21: return 'f';
        case 0x22: return 'g';
        case 0x23: return 'h';
        case 0x24: return 'j';
        case 0x25: return 'k';
        case 0x26: return 'l';
        case 0x2C: return 'z';
        case 0x2D: return 'x';
        case 0x2E: return 'c';
        case 0x2F: return 'v';
        case 0x30: return 'b';
        case 0x31: return 'n';
        case 0x32: return 'm';

        default:   return 0;
    }
}

// doomgeneric's contract is one event per DG_GetKey call, so events are
// buffered here and handed out one at a time. The kernel ring is drained in
// DG_DrawFrame - once per frame - because that is the only place the backend
// is guaranteed to be entered.
#define KEYQUEUE_SIZE 32

static unsigned short key_queue[KEYQUEUE_SIZE];
static unsigned int key_write_index;
static unsigned int key_read_index;

static void queue_key(int pressed, unsigned char doomkey)
{
    if (doomkey == 0) { return; }

    key_queue[key_write_index] = (unsigned short)((pressed << 8) | doomkey);
    key_write_index = (key_write_index + 1) % KEYQUEUE_SIZE;
}

static void drain_keyboard(void)
{
    int event;

    while ((event = secureos_getscancode()) >= 0) {
        unsigned code    = SECUREOS_KEY_CODE(event);
        int extended     = SECUREOS_KEY_IS_EXTENDED(event);
        int pressed      = !SECUREOS_KEY_IS_RELEASE(event);

        queue_key(pressed, scancode_to_doomkey(code, extended));
    }
}

void DG_Init(void)
{
    // Raw mode: presses and releases, arrows included, and stdin goes quiet so
    // nothing doom consumes reaches the shell's line buffer.
    if (secureos_kbd_raw(1) != 0) {
        printf("[DOOM] warning: raw keyboard mode refused, input will not work\n");
    }
}

void DG_DrawFrame(void)
{
    secureos_fb_present((const uint32_t *)DG_ScreenBuffer,
                        DOOMGENERIC_RESX, DOOMGENERIC_RESY);
    drain_keyboard();
}

void DG_SleepMs(uint32_t ms)
{
    if (ms == 0) { return; }
    __sys1(SYS_sleep, ms);
}

uint32_t DG_GetTicksMs(void)
{
    long ms = __sys1(SYS_gettime, 0);
    return ms < 0 ? 0 : (uint32_t)ms;
}

int DG_GetKey(int *pressed, unsigned char *doomkey)
{
    if (key_read_index == key_write_index) {
        return 0;
    }

    unsigned short entry = key_queue[key_read_index];
    key_read_index = (key_read_index + 1) % KEYQUEUE_SIZE;

    *pressed = entry >> 8;
    *doomkey = (unsigned char)(entry & 0xFF);
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

// The kernel puts no argv on the user stack and crt0.S synthesises argc=1,
// argv[0]="prog", so the WAD has to be named here. -iwad rather than relying
// on d_iwad.c's search, which walks directories this filesystem does not have.
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    char *doom_argv[] = { "doom", "-iwad", "/DOOM1.WAD", NULL };

    doomgeneric_Create(3, doom_argv);

    for (;;) {
        doomgeneric_Tick();
    }

    return 0;
}
