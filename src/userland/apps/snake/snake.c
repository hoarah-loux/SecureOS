/* A small framebuffer snake game for SecureOS userland. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "secureos_fb.h"
#include "secureos_kbd.h"
#include "secureos_syscall.h"

#define SCREEN_W 320
#define SCREEN_H 200
#define CELL 8
#define BOARD_W (SCREEN_W / CELL)
#define BOARD_H 22
#define BOARD_Y 24
#define MAX_SNAKE (BOARD_W * BOARD_H)
#define MOVE_MS 120
#define FRAME_MS 20

#define BLACK 0x081018u
#define GRID 0x142532u
#define BORDER 0x4C6A78u
#define SNAKE 0x4DDB8Au
#define HEAD 0xB7F36Bu
#define FOOD 0xFF6B6Bu
#define TEXT 0xD7E8E8u

struct point {
    int x;
    int y;
};

static uint32_t next_random(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static void fill_rect(uint32_t *pixels, int x, int y, int width, int height,
                      uint32_t colour)
{
    for (int row = y; row < y + height; row++) {
        for (int column = x; column < x + width; column++) {
            if (column >= 0 && column < SCREEN_W && row >= 0 && row < SCREEN_H) {
                pixels[row * SCREEN_W + column] = colour;
            }
        }
    }
}

static void draw_digit(uint32_t *pixels, int digit, int x, int y)
{
    static const uint8_t digits[10][5] = {
        {0x7, 0x5, 0x5, 0x5, 0x7}, {0x2, 0x6, 0x2, 0x2, 0x7},
        {0x7, 0x1, 0x7, 0x4, 0x7}, {0x7, 0x1, 0x7, 0x1, 0x7},
        {0x5, 0x5, 0x7, 0x1, 0x1}, {0x7, 0x4, 0x7, 0x1, 0x7},
        {0x7, 0x4, 0x7, 0x5, 0x7}, {0x7, 0x1, 0x1, 0x1, 0x1},
        {0x7, 0x5, 0x7, 0x5, 0x7}, {0x7, 0x5, 0x7, 0x1, 0x7}
    };

    if (digit < 0 || digit > 9) return;
    for (int row = 0; row < 5; row++) {
        for (int column = 0; column < 3; column++) {
            if (digits[digit][row] & (1u << (2 - column))) {
                fill_rect(pixels, x + column * 3, y + row * 3, 3, 3, TEXT);
            }
        }
    }
}

static int occupies(const struct point *snake, int length, struct point spot)
{
    for (int index = 0; index < length; index++) {
        if (snake[index].x == spot.x && snake[index].y == spot.y) return 1;
    }
    return 0;
}

static void place_food(struct point *food, const struct point *snake, int length,
                       uint32_t *random_state)
{
    do {
        food->x = (int)(next_random(random_state) % BOARD_W);
        food->y = (int)(next_random(random_state) % BOARD_H);
    } while (occupies(snake, length, *food));
}

static void draw_game(uint32_t *pixels, const struct point *snake, int length,
                      struct point food, unsigned score)
{
    for (int index = 0; index < SCREEN_W * SCREEN_H; index++) pixels[index] = BLACK;

    fill_rect(pixels, 0, 0, SCREEN_W, BOARD_Y, 0x102B35u);
    fill_rect(pixels, 0, BOARD_Y, SCREEN_W, 2, BORDER);
    draw_digit(pixels, (int)((score / 10) % 10), 10, 7);
    draw_digit(pixels, (int)(score % 10), 22, 7);

    for (int x = 0; x <= BOARD_W; x++) {
        fill_rect(pixels, x * CELL, BOARD_Y, 1, BOARD_H * CELL, GRID);
    }
    for (int y = 0; y <= BOARD_H; y++) {
        fill_rect(pixels, 0, BOARD_Y + y * CELL, SCREEN_W, 1, GRID);
    }
    fill_rect(pixels, 0, BOARD_Y, SCREEN_W, 2, BORDER);
    fill_rect(pixels, 0, BOARD_Y + BOARD_H * CELL - 2, SCREEN_W, 2, BORDER);

    fill_rect(pixels, food.x * CELL + 1, BOARD_Y + food.y * CELL + 1,
              CELL - 2, CELL - 2, FOOD);
    for (int index = length - 1; index >= 0; index--) {
        uint32_t colour = index == 0 ? HEAD : SNAKE;
        fill_rect(pixels, snake[index].x * CELL + 1,
                  BOARD_Y + snake[index].y * CELL + 1, CELL - 2, CELL - 2, colour);
    }
}

static void set_direction(int *direction_x, int *direction_y, int new_x, int new_y)
{
    if (new_x != -*direction_x || new_y != -*direction_y) {
        *direction_x = new_x;
        *direction_y = new_y;
    }
}

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    uint32_t *pixels = malloc((size_t)SCREEN_W * SCREEN_H * sizeof(*pixels));
    if (!pixels) {
        printf("SNAKE: framebuffer allocation failed\n");
        return 1;
    }
    if (secureos_kbd_raw(1) != 0) {
        printf("SNAKE: raw keyboard unavailable\n");
        free(pixels);
        return 1;
    }

    struct point snake[MAX_SNAKE];
    int length = 4;
    snake[0] = (struct point){BOARD_W / 2, BOARD_H / 2};
    snake[1] = (struct point){snake[0].x - 1, snake[0].y};
    snake[2] = (struct point){snake[0].x - 2, snake[0].y};
    snake[3] = (struct point){snake[0].x - 3, snake[0].y};
    struct point food;
    uint32_t random_state = 0x534E414Bu;
    place_food(&food, snake, length, &random_state);
    int direction_x = 1;
    int direction_y = 0;
    unsigned score = 0;
    long last_move = __sys0(SYS_gettime);
    int running = 1;
    int game_over = 0;

    while (running) {
        int event;
        while ((event = secureos_getscancode()) >= 0) {
            unsigned code = SECUREOS_KEY_CODE(event);
            int extended = SECUREOS_KEY_IS_EXTENDED(event);
            if (SECUREOS_KEY_IS_RELEASE(event)) continue;
            if (!extended && code == 0x01) {
                running = 0;
                continue;
            }
            if (extended && code == 0x48) set_direction(&direction_x, &direction_y, 0, -1);
            if (extended && code == 0x50) set_direction(&direction_x, &direction_y, 0, 1);
            if (extended && code == 0x4B) set_direction(&direction_x, &direction_y, -1, 0);
            if (extended && code == 0x4D) set_direction(&direction_x, &direction_y, 1, 0);
            if (!extended && code == 0x11) set_direction(&direction_x, &direction_y, 0, -1);
            if (!extended && code == 0x1F) set_direction(&direction_x, &direction_y, 0, 1);
            if (!extended && code == 0x1E) set_direction(&direction_x, &direction_y, -1, 0);
            if (!extended && code == 0x20) set_direction(&direction_x, &direction_y, 1, 0);
        }

        long now = __sys0(SYS_gettime);
        if (!game_over && now - last_move >= MOVE_MS) {
            last_move = now;
            struct point next = {snake[0].x + direction_x, snake[0].y + direction_y};
            int ate = next.x == food.x && next.y == food.y;
            int next_length = ate ? length + 1 : length;
            if (next.x < 0 || next.x >= BOARD_W || next.y < 0 || next.y >= BOARD_H ||
                occupies(snake, ate ? length : length - 1, next)) {
                game_over = 1;
            } else {
                if (next_length > MAX_SNAKE) next_length = MAX_SNAKE;
                for (int index = next_length - 1; index > 0; index--) snake[index] = snake[index - 1];
                snake[0] = next;
                length = next_length;
                if (ate) {
                    score++;
                    if (length < MAX_SNAKE) place_food(&food, snake, length, &random_state);
                }
            }
        }

        draw_game(pixels, snake, length, food, score);
        if (game_over) {
            fill_rect(pixels, 122, 94, 76, 12, 0x7A3038u);
            fill_rect(pixels, 128, 98, 64, 4, TEXT);
        }
        if (secureos_fb_present(pixels, SCREEN_W, SCREEN_H) != 0) break;
        __sys1(SYS_sleep, FRAME_MS);
    }

    secureos_kbd_raw(0);
    free(pixels);
    return 0;
}
