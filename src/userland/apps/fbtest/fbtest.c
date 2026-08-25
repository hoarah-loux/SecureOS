/* src/userland/apps/fbtest/fbtest.c
 *
 * A small, deliberately colourful SYS_fb_present demo. It also keeps the
 * pointer and extent checks that make this a useful framebuffer smoke test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "secureos_fb.h"
#include "secureos_syscall.h"

#define W 320
#define H 200
#define SHOW_MS 9000
#define FRAME_MS 40

static const uint8_t font[26][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,14}, {17,17,17,31,17,17,17},
    {31,4,4,4,4,4,31}, {7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
};

static uint32_t rainbow(unsigned hue)
{
    hue %= 768;
    if (hue < 128) return 0xFF0000u | ((hue * 2) << 8);
    if (hue < 256) return ((255u - (hue - 128) * 2) << 16) | 0x00FF00u;
    if (hue < 384) return 0x00FF00u | ((hue - 256) * 2);
    if (hue < 512) return 0x0000FFu | ((255u - (hue - 384) * 2) << 8);
    if (hue < 640) return ((hue - 512) * 2 << 16) | 0x0000FFu;
    return 0xFF0000u | ((255u - (hue - 640) * 2) << 8);
}

static const uint8_t *glyph(char ch)
{
    static const uint8_t star[7] = {4,21,14,31,14,21,4};
    static const uint8_t blank[7] = {0,0,0,0,0,0,0};
    if (ch == '*') return star;
    if (ch < 'A' || ch > 'Z') return blank;
    return font[(int)(ch - 'A')];
}

static void draw(uint32_t *px, unsigned frame)
{
    const char *message = "SECUREOS   *   RAINBOW MODE   *   ";
    const int scale = 3;
    const int text_y = 82;
    int scroll = W - (int)(frame * 3);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            unsigned hue = (unsigned)(x * 3 + y * 2 + frame * 5);
            uint32_t colour = rainbow(hue);
            unsigned wave = (unsigned)((x + frame * 4 + y * 3) % 96);

            /* Darken the rainbow into a deep space backdrop, then add two
             * bright diagonal ribbons that drift in opposite directions. */
            colour = ((colour >> 2) & 0x3F3F3Fu);
            if (wave < 5 || (wave > 48 && wave < 52)) {
                colour = rainbow(hue + 180);
            }
            if (((x * 7 + y * 11 + frame * 13) % 211) == 0) {
                colour = 0xFFFFFFu;
            }
            px[y * W + x] = colour;
        }
    }

    /* A large message crosses the whole source framebuffer, with each glyph
     * carrying a different moving slice of the rainbow. */
    for (int index = 0; message[index] != '\0'; index++) {
        int glyph_x = scroll + index * 18;
        const uint8_t *rows = glyph(message[index]);
        for (int gy = 0; gy < 7; gy++) {
            for (int gx = 0; gx < 5; gx++) {
                if (!(rows[gy] & (1u << (4 - gx)))) continue;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int x = glyph_x + gx * scale + sx;
                        int y = text_y + gy * scale + sy;
                        if (x >= 0 && x < W && y >= 0 && y < H) {
                            px[y * W + x] = rainbow((unsigned)(x * 5 + frame * 9));
                        }
                    }
                }
            }
        }
    }

    /* A crisp border makes the full source rectangle visible in screenshots. */
    for (int x = 0; x < W; x++) {
        px[x] = rainbow((unsigned)(x * 4 + frame * 5));
        px[(H - 1) * W + x] = rainbow((unsigned)(x * 4 + frame * 5 + 384));
    }
    for (int y = 0; y < H; y++) {
        px[y * W] = rainbow((unsigned)(y * 4 + frame * 5 + 192));
        px[y * W + W - 1] = rainbow((unsigned)(y * 4 + frame * 5 + 576));
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
    printf("[FBTEST] buffer %dx%d at %p\n", W, H, (void *)px);

    /* A kernel pointer must be refused rather than drawn. */
    if (secureos_fb_present((const uint32_t *)0xFFFFFFFF80000000ULL, W, H) != -1) {
        printf("[FBTEST] FAILED, kernel address was accepted\n");
        return 1;
    }
    printf("[FBTEST] kernel address rejected\n");

    /* So must a buffer whose start is mapped but whose end is not. 16384x16384
     * is inside the per-side limit the kernel enforces, so this gets past the
     * cheap checks and is caught by the region lookup on the last byte: one
     * gigabyte from px runs off the end of the 64 MiB heap arena. */
    if (secureos_fb_present(px, 16384u, 16384u) != -1) {
        printf("[FBTEST] FAILED, unmapped extent was accepted\n");
        return 1;
    }
    printf("[FBTEST] unmapped extent rejected\n");

    if (secureos_fb_present(px, 0, H) != -1) {
        printf("[FBTEST] FAILED, zero width was accepted\n");
        return 1;
    }
    printf("[FBTEST] zero width rejected\n");

    printf("[FBTEST] animation started for %d ms\n", SHOW_MS);
    long started = __sys0(SYS_gettime);
    unsigned frame = 0;
    while (__sys0(SYS_gettime) - started < SHOW_MS) {
        draw(px, frame++);
        if (secureos_fb_present(px, W, H) != 0) {
            printf("[FBTEST] FAILED, animation present returned -1\n");
            free(px);
            return 1;
        }
        __sys1(SYS_sleep, FRAME_MS);
    }
    printf("[FBTEST] animation complete after %u frames\n", frame);

    free(px);
    printf("[FBTEST] all steps OK, exiting 0\n");
    return 0;
}
