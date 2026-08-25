#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../libc_glue/secureos_fb.h"
#include "../../libc_glue/secureos_kbd.h"
#include "../../libc_glue/secureos_syscall.h"

#define SCREEN_W 320
#define SCREEN_H 200
#define FRAME_MS 16
#define FLASH_MS 120

#define MAX_TEXT 31

#define BODY_BG 0x1A2026u
#define BODY_EDGE 0x46515Du
#define PANEL_BG 0x2C3742u
#define DISPLAY_BG 0x0E1318u
#define DISPLAY_TEXT 0xD9F2A4u
#define LABEL_TEXT 0xAFC6D2u

struct button {
    const char *label;
    int x;
    int y;
    int w;
    int h;
    uint32_t base;
    uint32_t pressed;
    uint32_t text;
    long pressed_until;
};

enum button_id {
    BTN_CLEAR,
    BTN_BACK,
    BTN_DIV,
    BTN_MUL,
    BTN_7,
    BTN_8,
    BTN_9,
    BTN_SUB,
    BTN_4,
    BTN_5,
    BTN_6,
    BTN_ADD,
    BTN_1,
    BTN_2,
    BTN_3,
    BTN_EQ,
    BTN_SIGN,
    BTN_0,
    BTN_DOT,
    BTN_COUNT
};

struct calc_state {
    char display[MAX_TEXT + 1];
    double lhs;
    char pending_op;
    int entering_new;
    int error;
    int shift_down;
};

static void fill_rect(uint32_t *pixels, int x, int y, int w, int h, uint32_t color)
{
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            if (col >= 0 && col < SCREEN_W && row >= 0 && row < SCREEN_H) {
                pixels[row * SCREEN_W + col] = color;
            }
        }
    }
}

static uint32_t dim_color(uint32_t c, unsigned pct)
{
    unsigned r = ((c >> 16) & 0xFFu) * pct / 100u;
    unsigned g = ((c >> 8) & 0xFFu) * pct / 100u;
    unsigned b = (c & 0xFFu) * pct / 100u;
    return (r << 16) | (g << 8) | b;
}

static const uint8_t *glyph_for(char ch)
{
    static const uint8_t g0[7] = {14,17,19,21,25,17,14};
    static const uint8_t g1[7] = {4,12,4,4,4,4,14};
    static const uint8_t g2[7] = {14,17,1,2,4,8,31};
    static const uint8_t g3[7] = {30,1,1,14,1,1,30};
    static const uint8_t g4[7] = {2,6,10,18,31,2,2};
    static const uint8_t g5[7] = {31,16,16,30,1,1,30};
    static const uint8_t g6[7] = {6,8,16,30,17,17,14};
    static const uint8_t g7[7] = {31,1,2,4,8,8,8};
    static const uint8_t g8[7] = {14,17,17,14,17,17,14};
    static const uint8_t g9[7] = {14,17,17,15,1,2,12};
    static const uint8_t ga[7] = {14,17,17,31,17,17,17};
    static const uint8_t gb[7] = {30,17,17,30,17,17,30};
    static const uint8_t gc[7] = {14,17,16,16,16,17,14};
    static const uint8_t gd[7] = {28,18,17,17,17,18,28};
    static const uint8_t ge[7] = {31,16,16,30,16,16,31};
    static const uint8_t gl[7] = {16,16,16,16,16,16,31};
    static const uint8_t go[7] = {14,17,17,17,17,17,14};
    static const uint8_t gr[7] = {30,17,17,30,20,18,17};
    static const uint8_t gs[7] = {15,16,16,14,1,1,30};
    static const uint8_t gt[7] = {31,4,4,4,4,4,4};
    static const uint8_t gx[7] = {17,17,10,4,10,17,17};
    static const uint8_t gplus[7] = {0,4,4,31,4,4,0};
    static const uint8_t gminus[7] = {0,0,0,31,0,0,0};
    static const uint8_t gmul[7] = {0,17,10,4,10,17,0};
    static const uint8_t gdiv[7] = {1,2,4,8,16,0,0};
    static const uint8_t geq[7] = {0,31,0,31,0,0,0};
    static const uint8_t gdot[7] = {0,0,0,0,0,12,12};
    static const uint8_t gblank[7] = {0,0,0,0,0,0,0};

    switch (ch) {
    case '0': return g0;
    case '1': return g1;
    case '2': return g2;
    case '3': return g3;
    case '4': return g4;
    case '5': return g5;
    case '6': return g6;
    case '7': return g7;
    case '8': return g8;
    case '9': return g9;
    case 'A': return ga;
    case 'B': return gb;
    case 'C': return gc;
    case 'D': return gd;
    case 'E': return ge;
    case 'L': return gl;
    case 'O': return go;
    case 'R': return gr;
    case 'S': return gs;
    case 'T': return gt;
    case 'X': return gx;
    case '+': return gplus;
    case '-': return gminus;
    case '*': return gmul;
    case '/': return gdiv;
    case '=': return geq;
    case '.': return gdot;
    case ' ': return gblank;
    default: return gblank;
    }
}

static void draw_text(uint32_t *pixels, const char *text, int x, int y,
                      int scale, uint32_t color)
{
    for (int index = 0; text[index] != '\0'; index++) {
        const uint8_t *g = glyph_for(text[index]);
        int ox = x + index * (6 * scale);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (g[row] & (1u << (4 - col))) {
                    fill_rect(pixels,
                              ox + col * scale,
                              y + row * scale,
                              scale,
                              scale,
                              color);
                }
            }
        }
    }
}

static void format_value(double value, char *out)
{
    snprintf(out, MAX_TEXT + 1, "%.10g", value);
}

static void calc_reset(struct calc_state *state)
{
    strcpy(state->display, "0");
    state->lhs = 0.0;
    state->pending_op = 0;
    state->entering_new = 1;
    state->error = 0;
    state->shift_down = 0;
}

static double current_value(const struct calc_state *state)
{
    return strtod(state->display, NULL);
}

static void set_error(struct calc_state *state)
{
    strcpy(state->display, "ERROR");
    state->pending_op = 0;
    state->entering_new = 1;
    state->error = 1;
}

static void input_digit(struct calc_state *state, char digit)
{
    if (state->error) {
        calc_reset(state);
    }

    if (state->entering_new || strcmp(state->display, "0") == 0) {
        state->display[0] = digit;
        state->display[1] = '\0';
        state->entering_new = 0;
        return;
    }

    size_t len = strlen(state->display);
    if (len < MAX_TEXT) {
        state->display[len] = digit;
        state->display[len + 1] = '\0';
    }
}

static void input_dot(struct calc_state *state)
{
    if (state->error) {
        calc_reset(state);
    }

    if (state->entering_new) {
        strcpy(state->display, "0.");
        state->entering_new = 0;
        return;
    }

    if (strchr(state->display, '.') == NULL) {
        size_t len = strlen(state->display);
        if (len < MAX_TEXT) {
            state->display[len] = '.';
            state->display[len + 1] = '\0';
        }
    }
}

static void backspace(struct calc_state *state)
{
    if (state->error) {
        calc_reset(state);
        return;
    }

    if (state->entering_new) {
        return;
    }

    size_t len = strlen(state->display);
    if (len > 1) {
        state->display[len - 1] = '\0';
    } else {
        strcpy(state->display, "0");
        state->entering_new = 1;
    }
}

static void toggle_sign(struct calc_state *state)
{
    if (state->error) {
        calc_reset(state);
    }

    if (strcmp(state->display, "0") == 0 || state->entering_new) {
        return;
    }

    if (state->display[0] == '-') {
        memmove(state->display, state->display + 1, strlen(state->display));
    } else {
        size_t len = strlen(state->display);
        if (len < MAX_TEXT) {
            memmove(state->display + 1, state->display, len + 1);
            state->display[0] = '-';
        }
    }
}

static int apply_binary(double lhs, double rhs, char op, double *out)
{
    switch (op) {
    case '+': *out = lhs + rhs; return 1;
    case '-': *out = lhs - rhs; return 1;
    case '*': *out = lhs * rhs; return 1;
    case '/':
        if (rhs == 0.0) {
            return 0;
        }
        *out = lhs / rhs;
        return 1;
    default:
        return 0;
    }
}

static void apply_operator(struct calc_state *state, char op)
{
    if (state->error) {
        return;
    }

    double rhs = current_value(state);

    if (state->pending_op != 0 && !state->entering_new) {
        double result = 0.0;
        if (!apply_binary(state->lhs, rhs, state->pending_op, &result)) {
            set_error(state);
            return;
        }
        state->lhs = result;
        format_value(result, state->display);
    } else if (state->pending_op == 0) {
        state->lhs = rhs;
    }

    state->pending_op = op;
    state->entering_new = 1;
}

static void apply_equals(struct calc_state *state)
{
    if (state->error || state->pending_op == 0) {
        return;
    }

    double rhs = current_value(state);
    double result = 0.0;
    if (!apply_binary(state->lhs, rhs, state->pending_op, &result)) {
        set_error(state);
        return;
    }

    state->lhs = result;
    state->pending_op = 0;
    state->entering_new = 1;
    format_value(result, state->display);
}

static void init_buttons(struct button *buttons)
{
    int x0 = 72;
    int y0 = 64;
    int bw = 38;
    int bh = 22;
    int gx = 8;
    int gy = 6;

    buttons[BTN_CLEAR] = (struct button){"C", x0, y0, bw, bh, 0xB14C4Cu, 0xE36E6Eu, 0xFFFFFFu, 0};
    buttons[BTN_BACK]  = (struct button){"BS", x0 + (bw + gx), y0, bw, bh, 0x6C5F8Fu, 0x9183BCu, 0xFFFFFFu, 0};
    buttons[BTN_DIV]   = (struct button){"/", x0 + 2 * (bw + gx), y0, bw, bh, 0x4F7F9Cu, 0x6FA7C8u, 0xFFFFFFu, 0};
    buttons[BTN_MUL]   = (struct button){"*", x0 + 3 * (bw + gx), y0, bw, bh, 0x4F7F9Cu, 0x6FA7C8u, 0xFFFFFFu, 0};

    buttons[BTN_7]     = (struct button){"7", x0, y0 + (bh + gy), bw, bh, 0x415364u, 0x5A7187u, 0xF7FAFCu, 0};
    buttons[BTN_8]     = (struct button){"8", x0 + (bw + gx), y0 + (bh + gy), bw, bh, 0x415364u, 0x5A7187u, 0xF7FAFCu, 0};
    buttons[BTN_9]     = (struct button){"9", x0 + 2 * (bw + gx), y0 + (bh + gy), bw, bh, 0x415364u, 0x5A7187u, 0xF7FAFCu, 0};
    buttons[BTN_SUB]   = (struct button){"-", x0 + 3 * (bw + gx), y0 + (bh + gy), bw, bh, 0x4F7F9Cu, 0x6FA7C8u, 0xFFFFFFu, 0};

    buttons[BTN_4]     = (struct button){"4", x0, y0 + 2 * (bh + gy), bw, bh, 0x415364u, 0x5A7187u, 0xF7FAFCu, 0};
    buttons[BTN_5]     = (struct button){"5", x0 + (bw + gx), y0 + 2 * (bh + gy), bw, bh, 0x415364u, 0x5A7187u, 0xF7FAFCu, 0};
    buttons[BTN_6]     = (struct button){"6", x0 + 2 * (bw + gx), y0 + 2 * (bh + gy), bw, bh, 0x415364u, 0x5A7187u, 0xF7FAFCu, 0};
    buttons[BTN_ADD]   = (struct button){"+", x0 + 3 * (bw + gx), y0 + 2 * (bh + gy), bw, bh, 0x4F7F9Cu, 0x6FA7C8u, 0xFFFFFFu, 0};

    buttons[BTN_1]     = (struct button){"1", x0, y0 + 3 * (bh + gy), bw, bh, 0x415364u, 0x5A7187u, 0xF7FAFCu, 0};
    buttons[BTN_2]     = (struct button){"2", x0 + (bw + gx), y0 + 3 * (bh + gy), bw, bh, 0x415364u, 0x5A7187u, 0xF7FAFCu, 0};
    buttons[BTN_3]     = (struct button){"3", x0 + 2 * (bw + gx), y0 + 3 * (bh + gy), bw, bh, 0x415364u, 0x5A7187u, 0xF7FAFCu, 0};
    buttons[BTN_EQ]    = (struct button){"=", x0 + 3 * (bw + gx), y0 + 3 * (bh + gy), bw, bh * 2 + gy, 0xD28A3Du, 0xF0AB5Au, 0x1C1A12u, 0};

    buttons[BTN_SIGN]  = (struct button){"S", x0, y0 + 4 * (bh + gy), bw, bh, 0x5E6B77u, 0x788998u, 0xF7FAFCu, 0};
    buttons[BTN_0]     = (struct button){"0", x0 + (bw + gx), y0 + 4 * (bh + gy), bw, bh, 0x415364u, 0x5A7187u, 0xF7FAFCu, 0};
    buttons[BTN_DOT]   = (struct button){".", x0 + 2 * (bw + gx), y0 + 4 * (bh + gy), bw, bh, 0x415364u, 0x5A7187u, 0xF7FAFCu, 0};
}

static void draw_button(uint32_t *pixels, const struct button *button, long now)
{
    int pushed = now < button->pressed_until;
    uint32_t top = pushed ? dim_color(button->pressed, 70) : button->pressed;
    uint32_t face = pushed ? button->pressed : button->base;
    uint32_t edge = pushed ? dim_color(button->base, 55) : dim_color(button->base, 75);

    fill_rect(pixels, button->x, button->y, button->w, button->h, edge);
    fill_rect(pixels, button->x + 1, button->y + 1, button->w - 2, 2, top);
    fill_rect(pixels, button->x + 1, button->y + 3, button->w - 2, button->h - 4, face);

    int scale = (button->w >= 38) ? 2 : 1;
    int text_w = (int)strlen(button->label) * 6 * scale - scale;
    int text_h = 7 * scale;
    int tx = button->x + (button->w - text_w) / 2 + (pushed ? 1 : 0);
    int ty = button->y + (button->h - text_h) / 2 + (pushed ? 1 : 0);
    draw_text(pixels, button->label, tx, ty, scale, button->text);
}

static void draw_ui(uint32_t *pixels,
                    const struct button *buttons,
                    const struct calc_state *state,
                    long now)
{
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        pixels[i] = 0x0C1319u;
    }

    for (int y = 0; y < SCREEN_H; y++) {
        uint32_t shade = (uint32_t)(10 + (y * 24 / SCREEN_H));
        uint32_t bg = (shade << 16) | ((shade + 8) << 8) | (shade + 14);
        fill_rect(pixels, 0, y, SCREEN_W, 1, bg);
    }

    fill_rect(pixels, 56, 6, 208, 188, BODY_EDGE);
    fill_rect(pixels, 58, 8, 204, 184, BODY_BG);
    fill_rect(pixels, 62, 12, 196, 176, PANEL_BG);

    fill_rect(pixels, 70, 18, 180, 38, BODY_EDGE);
    fill_rect(pixels, 72, 20, 176, 34, DISPLAY_BG);

    const char *op = " ";
    if (state->pending_op != 0) {
        static char op_buf[2];
        op_buf[0] = state->pending_op;
        op_buf[1] = '\0';
        op = op_buf;
    }
    draw_text(pixels, op, 76, 24, 1, LABEL_TEXT);

    int display_scale = 2;
    int len = (int)strlen(state->display);
    int text_w = len * 6 * display_scale - display_scale;
    if (text_w > 166) {
        display_scale = 1;
        text_w = len * 6 * display_scale - display_scale;
    }
    draw_text(pixels,
              state->display,
              244 - text_w,
              31 - (display_scale == 2 ? 7 : 3),
              display_scale,
              DISPLAY_TEXT);

    draw_text(pixels, "ESC", 74, 182, 1, LABEL_TEXT);
    draw_text(pixels, "TO EXIT", 92, 182, 1, LABEL_TEXT);

    for (int id = 0; id < BTN_COUNT; id++) {
        draw_button(pixels, &buttons[id], now);
    }
}

static int map_key_to_button(unsigned code, int extended, int shift_down)
{
    (void)extended;

    if (code >= 0x02 && code <= 0x0A) {
        static const int top_row_digit[] = {
            BTN_1, BTN_2, BTN_3, BTN_4, BTN_5, BTN_6, BTN_7, BTN_8, BTN_9
        };
        return top_row_digit[code - 0x02];
    }

    switch (code) {
    case 0x0B: return BTN_0;
    case 0x0E: return BTN_BACK;
    case 0x1C: return BTN_EQ;
    case 0x2E: return BTN_CLEAR;  /* C */
    case 0x31: return BTN_SIGN;   /* N for negation */
    case 0x34: return BTN_DOT;
    case 0x35: return BTN_DIV;
    case 0x37: return BTN_MUL;
    case 0x47: return BTN_7;
    case 0x48: return BTN_8;
    case 0x49: return BTN_9;
    case 0x4A: return BTN_SUB;
    case 0x4B: return BTN_4;
    case 0x4C: return BTN_5;
    case 0x4D: return BTN_6;
    case 0x4E: return BTN_ADD;
    case 0x4F: return BTN_1;
    case 0x50: return BTN_2;
    case 0x51: return BTN_3;
    case 0x52: return BTN_0;
    case 0x53:
        if (extended) {
            return BTN_CLEAR;
        }
        return BTN_DOT;
    case 0x0C: return BTN_SUB;
    case 0x0D:
        return shift_down ? BTN_ADD : BTN_EQ;
    default:
        return -1;
    }
}

static void handle_button(struct calc_state *state, int button)
{
    switch (button) {
    case BTN_0: input_digit(state, '0'); break;
    case BTN_1: input_digit(state, '1'); break;
    case BTN_2: input_digit(state, '2'); break;
    case BTN_3: input_digit(state, '3'); break;
    case BTN_4: input_digit(state, '4'); break;
    case BTN_5: input_digit(state, '5'); break;
    case BTN_6: input_digit(state, '6'); break;
    case BTN_7: input_digit(state, '7'); break;
    case BTN_8: input_digit(state, '8'); break;
    case BTN_9: input_digit(state, '9'); break;
    case BTN_DOT: input_dot(state); break;
    case BTN_ADD: apply_operator(state, '+'); break;
    case BTN_SUB: apply_operator(state, '-'); break;
    case BTN_MUL: apply_operator(state, '*'); break;
    case BTN_DIV: apply_operator(state, '/'); break;
    case BTN_EQ: apply_equals(state); break;
    case BTN_CLEAR: calc_reset(state); break;
    case BTN_BACK: backspace(state); break;
    case BTN_SIGN: toggle_sign(state); break;
    default: break;
    }
}

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    uint32_t *pixels = malloc((size_t)SCREEN_W * SCREEN_H * sizeof(*pixels));
    if (!pixels) {
        printf("CALC: framebuffer allocation failed\n");
        return 1;
    }

    if (secureos_kbd_raw(1) != 0) {
        printf("CALC: raw keyboard unavailable\n");
        free(pixels);
        return 1;
    }

    struct calc_state state;
    calc_reset(&state);

    struct button buttons[BTN_COUNT];
    init_buttons(buttons);

    int running = 1;
    while (running) {
        int event;
        long now = __sys0(SYS_gettime);

        while ((event = secureos_getscancode()) >= 0) {
            unsigned code = SECUREOS_KEY_CODE(event);
            int extended = SECUREOS_KEY_IS_EXTENDED(event);
            int released = SECUREOS_KEY_IS_RELEASE(event);

            if (!extended && (code == 0x2A || code == 0x36)) {
                state.shift_down = !released;
                continue;
            }

            if (released) {
                continue;
            }

            if (!extended && code == 0x01) {
                running = 0;
                continue;
            }

            int button = map_key_to_button(code, extended, state.shift_down);
            if (button >= 0 && button < BTN_COUNT) {
                buttons[button].pressed_until = now + FLASH_MS;
                handle_button(&state, button);
            }
        }

        draw_ui(pixels, buttons, &state, now);
        if (secureos_fb_present(pixels, SCREEN_W, SCREEN_H) != 0) {
            printf("CALC: framebuffer present failed\n");
            break;
        }

        __sys1(SYS_sleep, FRAME_MS);
    }

    secureos_kbd_raw(0);
    free(pixels);
    return 0;
}