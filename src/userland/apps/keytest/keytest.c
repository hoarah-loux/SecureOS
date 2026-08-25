/* src/userland/apps/keytest/keytest.c
 *
 * Raw keyboard events from ring3 - milestone 5 of the doom port.
 *
 * What the cooked path cannot do, and this exists to prove it now can:
 *
 *   - report a key *release*. Cooked mode drops every scancode with the top
 *     bit set, so nothing could ever tell "still held" from "let go", which is
 *     most of what a game needs from a keyboard.
 *   - report the extended keys. Arrows, RCtrl, RAlt and keypad Enter arrive
 *     behind an 0xE0 prefix that cooked mode has no handling for at all, so as
 *     far as a program was concerned they did not exist.
 *   - keep those keystrokes away from the shell. In raw mode the cooked ring
 *     is not fed, so nothing the player types piles up to be executed as a
 *     command the moment the game exits.
 *
 * Prints every event it sees, then a self-check. Press Escape to finish; it
 * also gives up on its own after a while so a headless run cannot hang.
 */

#include <stdio.h>
#include <stdlib.h>

#include "secureos_kbd.h"
#include "secureos_syscall.h"

#define SC_ESCAPE 0x01
#define TIMEOUT_MS 20000
#define MAX_EVENTS 64

static const char *key_name(unsigned code, int extended)
{
    if (extended) {
        switch (code) {
        case 0x48: return "Up";
        case 0x50: return "Down";
        case 0x4B: return "Left";
        case 0x4D: return "Right";
        case 0x1D: return "RCtrl";
        case 0x38: return "RAlt";
        case 0x1C: return "KeypadEnter";
        default:   return "extended";
        }
    }

    switch (code) {
    case 0x01: return "Escape";
    case 0x1C: return "Enter";
    case 0x1D: return "LCtrl";
    case 0x2A: return "LShift";
    case 0x38: return "LAlt";
    case 0x39: return "Space";
    default:   return "key";
    }
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    if (secureos_kbd_raw(1) != 0) {
        printf("[KEYTEST] FAILED: could not enter raw mode\n");
        return 1;
    }
    printf("[KEYTEST] raw mode on; press keys, Escape to finish\n");

    unsigned presses = 0, releases = 0, extended_seen = 0, matched_pairs = 0;
    unsigned pressed_codes[MAX_EVENTS];
    unsigned pressed_count = 0;
    unsigned total = 0;

    long start = __sys0(SYS_gettime);
    int done = 0;

    while (!done) {
        int ev = secureos_getscancode();

        if (ev < 0) {
            if (__sys0(SYS_gettime) - start > TIMEOUT_MS) {
                printf("[KEYTEST] timed out waiting for input\n");
                break;
            }
            __sys1(SYS_sleep, 10);
            continue;
        }

        unsigned code = SECUREOS_KEY_CODE(ev);
        int extended = SECUREOS_KEY_IS_EXTENDED(ev);
        int release = SECUREOS_KEY_IS_RELEASE(ev);

        if (total < MAX_EVENTS) {
            printf("[KEYTEST] event 0x%04X  %-12s %s%s\n",
                   (unsigned)ev, key_name(code, extended),
                   release ? "release" : "press",
                   extended ? " (extended)" : "");
            total++;
        }

        if (extended) { extended_seen++; }

        if (release) {
            releases++;

            /* Did we see this exact key go down earlier? */
            for (unsigned i = 0; i < pressed_count; i++) {
                if (pressed_codes[i] == (unsigned)(ev & ~SECUREOS_KEY_RELEASED)) {
                    matched_pairs++;
                    break;
                }
            }

            if (!extended && code == SC_ESCAPE) {
                done = 1;
            }
        } else {
            presses++;
            if (pressed_count < MAX_EVENTS) {
                pressed_codes[pressed_count++] = (unsigned)ev;
            }
        }
    }

    if (secureos_kbd_raw(0) != 0) {
        printf("[KEYTEST] FAILED: could not leave raw mode\n");
        return 1;
    }

    printf("[KEYTEST] %u presses, %u releases, %u extended, %u matched pairs\n",
           presses, releases, extended_seen, matched_pairs);

    int failures = 0;

    if (presses == 0)       { printf("[KEYTEST] FAILED: no presses seen\n"); failures++; }
    if (releases == 0)      { printf("[KEYTEST] FAILED: no releases seen - cooked mode still filtering?\n"); failures++; }
    if (extended_seen == 0) { printf("[KEYTEST] FAILED: no extended keys seen - 0xE0 not decoded?\n"); failures++; }
    if (matched_pairs == 0) { printf("[KEYTEST] FAILED: no press/release pair for the same key\n"); failures++; }

    if (failures) {
        return 1;
    }

    printf("[KEYTEST] all checks OK, exiting 0\n");
    return 0;
}
