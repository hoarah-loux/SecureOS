/* src/userland/apps/fbtest/fbtest.c
 *
 * SYS_fb_present, at the resolution doom will use.
 *
 * The picture is deliberately made of flat colours with hard edges at known
 * coordinates rather than anything pretty: a screenshot can then be checked by
 * sampling a handful of pixels, and a wrong scale factor, a wrong centring
 * offset or a swapped colour channel each fail a specific assertion instead of
 * merely looking odd. The frames are:
 *
 *   1. four quadrants (red, green, blue, yellow), a white one-pixel border,
 *      and a black square dead centre
 *   2. the same with the quadrant colours rotated, to prove that a second
 *      present overwrites the first rather than blending or being ignored
 *
 * Every step prints to the serial port, so a headless run says where it got to.
 * The last frame is left on screen, which is what a screenshot captures.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "secureos_fb.h"
#include "secureos_syscall.h"

#define W 320
#define H 200

#define RED    0xFF0000u
#define GREEN  0x00FF00u
#define BLUE   0x0000FFu
#define YELLOW 0xFFFF00u
#define WHITE  0xFFFFFFu
#define BLACK  0x000000u

/* Half the side of the centre square. */
#define CENTRE_HALF 10

static void draw(uint32_t *px, uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint32_t c;

            if (x < W / 2) {
                c = (y < H / 2) ? tl : bl;
            } else {
                c = (y < H / 2) ? tr : br;
            }

            /* Centre square, then border, so the border always wins. */
            if (x >= W / 2 - CENTRE_HALF && x < W / 2 + CENTRE_HALF &&
                y >= H / 2 - CENTRE_HALF && y < H / 2 + CENTRE_HALF) {
                c = BLACK;
            }

            if (x == 0 || y == 0 || x == W - 1 || y == H - 1) {
                c = WHITE;
            }

            px[y * W + x] = c;
        }
    }
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    uint32_t *px = malloc((size_t)W * H * sizeof(uint32_t));
    if (!px) {
        printf("[FBTEST] malloc failed\n");
        return 1;
    }
    printf("[FBTEST] step 1: buffer %dx%d at %p\n", W, H, (void *)px);

    draw(px, RED, GREEN, BLUE, YELLOW);
    if (secureos_fb_present(px, W, H) != 0) {
        printf("[FBTEST] step 2: FAILED, present returned -1\n");
        return 1;
    }
    printf("[FBTEST] step 2: first frame presented\n");

    /* newlib's sleep() has no glue behind it yet, so go straight at SYS_sleep.
     * The pause exists only so a human watching the screen sees two distinct
     * frames rather than one. */
    __sys1(SYS_sleep, 500);

    draw(px, YELLOW, RED, GREEN, BLUE);
    if (secureos_fb_present(px, W, H) != 0) {
        printf("[FBTEST] step 3: FAILED, second present returned -1\n");
        return 1;
    }
    printf("[FBTEST] step 3: second frame presented\n");

    /* A kernel pointer must be refused rather than drawn. */
    if (secureos_fb_present((const uint32_t *)0xFFFFFFFF80000000ULL, W, H) != -1) {
        printf("[FBTEST] step 4: FAILED, kernel address was accepted\n");
        return 1;
    }
    printf("[FBTEST] step 4: kernel address rejected\n");

    /* So must a buffer whose start is mapped but whose end is not. 16384x16384
     * is inside the per-side limit the kernel enforces, so this gets past the
     * cheap checks and is caught by the region lookup on the last byte: one
     * gigabyte from px runs off the end of the 64 MiB heap arena. */
    if (secureos_fb_present(px, 16384u, 16384u) != -1) {
        printf("[FBTEST] step 5: FAILED, unmapped extent was accepted\n");
        return 1;
    }
    printf("[FBTEST] step 5: unmapped extent rejected\n");

    if (secureos_fb_present(px, 0, H) != -1) {
        printf("[FBTEST] step 6: FAILED, zero width was accepted\n");
        return 1;
    }
    printf("[FBTEST] step 6: zero width rejected\n");

    /* How fast is a present, really?
     *
     * This is the number milestone 4 of the doom port turns on: doom runs its
     * game logic at 35 Hz, so anything at or above that leaves the blit out of
     * the critical path, and anything well below it makes the blit the thing to
     * fix first. Measured over enough frames that the timer's 10 ms resolution
     * does not dominate. */
    {
        const int frames = 60;
        long t0 = __sys0(SYS_gettime);

        for (int i = 0; i < frames; i++) {
            secureos_fb_present(px, W, H);
        }

        long t1 = __sys0(SYS_gettime);
        long ms = t1 - t0;

        if (ms <= 0) {
            printf("[FBTEST] step 7: %d frames in under one tick\n", frames);
        } else {
            printf("[FBTEST] step 7: %d frames in %ld ms = %ld fps\n",
                   frames, ms, (long)frames * 1000 / ms);
        }
    }

    /* Hold the last frame. The shell writes to the console - which is the same
     * framebuffer - as soon as this process exits, so without a pause there is
     * nothing left to look at or to screenshot. */
    printf("[FBTEST] holding frame\n");
    __sys1(SYS_sleep, 3000);

    free(px);
    printf("[FBTEST] all steps OK, exiting 0\n");
    return 0;
}
