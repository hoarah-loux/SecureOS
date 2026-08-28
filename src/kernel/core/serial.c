#include <stddef.h>
#include <stdarg.h>
#include <serial.h>

enum {
    COM1 = 0x3F8,
};

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void serial_init(void) {
    /*
     * Safe to call even if the entry stub already initialized COM1.
     * 38400 baud, 8N1, FIFO on.
     */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

// Sticky "COM1 is not usable" latch. On hardware without a functioning
// UART the Line Status Register can read 0x00 forever (SuperIO serial
// absent or disabled - QEMU always emulates COM1, so emulator runs never
// see this), and the old unbounded THRE poll hung the whole kernel on the
// first printk with nothing on screen. One bounded timeout flips this
// flag and serial output becomes a no-op; the framebuffer console mirror
// is the output path that remains.
static int serial_dead;

void serial_write_char(char c) {
    if (serial_dead) {
        return;
    }

    // 65536 polls is orders of magnitude beyond what a live 38400-baud
    // UART needs to drain one byte.
    uint32_t spins = 65536;
    while ((inb(COM1 + 5) & 0x20) == 0) {
        if (--spins == 0) {
            serial_dead = 1;
            return;
        }
    }
    outb(COM1, (uint8_t)c);
}

static void mirrored_putc(char c);

void serial_write_string(const char *s) {
    while (*s) {
        mirrored_putc(*s++);
    }
}

void serial_write_hex32(uint32_t value) {
    for (int i = 0; i < 8; i++) {
        uint8_t nibble = (value >> 28) & 0xF;
        char c = (nibble <= 9) ? (char)('0' + nibble) : (char)('A' + (nibble - 10));
        mirrored_putc(c);
        value <<= 4;
    }
}

void serial_write_hex64(uint64_t value) {
    serial_write_hex32((uint32_t)(value >> 32));
    serial_write_hex32((uint32_t)(value & 0xFFFFFFFFu));
}

// See hdr/serial.h: printk's characters - and the hex/string log helpers
// above - fan out to the serial port and, once kmain registers the console,
// to the framebuffer as well. serial_write_char itself stays raw: sys_write
// calls it per character while also writing the console, and mirroring at
// that level would double userland's on-screen output. Runtime-assigned
// (not a static initializer), so the stored address is the post-transition
// higher-half one and needs no KIMG rebase.
static printk_mirror_fn printk_mirror;

void printk_set_mirror(printk_mirror_fn fn) {
    printk_mirror = fn;
}

static void mirrored_putc(char c) {
    serial_write_char(c);
    if (printk_mirror) {
        printk_mirror(c);
    }
}

static int serial_write_n(const char *s, size_t len) {
    int count = 0;
    for (size_t i = 0; i < len; i++) {
        mirrored_putc(s[i]);
        count++;
    }
    return count;
}

static int serial_write_repeat(char c, int count) {
    int written = 0;
    for (int i = 0; i < count; i++) {
        mirrored_putc(c);
        written++;
    }
    return written;
}

static int utoa_base(uintmax_t value, unsigned base, int uppercase, char *buf, int buf_len) {
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;

    if (buf_len <= 1) {
        return 0;
    }

    if (value == 0) {
        buf[i++] = '0';
        buf[i] = '\0';
        return i;
    }

    while (value != 0 && i < buf_len - 1) {
        buf[i++] = digits[value % base];
        value /= base;
    }

    for (int j = 0; j < i / 2; j++) {
        char tmp = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = tmp;
    }
    buf[i] = '\0';
    return i;
}

static int max_int(int a, int b) {
    return (a > b) ? a : b;
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

int printk(const char *fmt, ...) {
    enum {
        LEN_NONE,
        LEN_HH,
        LEN_H,
        LEN_L,
        LEN_LL,
        LEN_Z,
        LEN_T,
        LEN_J
    } length;

    int count = 0;
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            mirrored_putc(*fmt++);
            count++;
            continue;
        }

        fmt++;

        int left = 0;
        int plus = 0;
        int space = 0;
        int alt = 0;
        int zero = 0;

        int width = 0;
        int precision = -1;

        length = LEN_NONE;

        int done = 0;
        while (!done) {
            switch (*fmt) {
                case '-': left = 1; fmt++; break;
                case '+': plus = 1; fmt++; break;
                case ' ': space = 1; fmt++; break;
                case '#': alt = 1; fmt++; break;
                case '0': zero = 1; fmt++; break;
                default: done = 1; break;
            }
        }

        if (*fmt == '*') {
            width = va_arg(args, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            fmt++;
        } else {
            while (is_digit(*fmt)) {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(args, int);
                if (precision < 0) {
                    precision = -1;
                }
                fmt++;
            } else {
                while (is_digit(*fmt)) {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        if (*fmt == 'h' && *(fmt + 1) == 'h') {
            length = LEN_HH;
            fmt += 2;
        } else if (*fmt == 'h') {
            length = LEN_H;
            fmt++;
        } else if (*fmt == 'l' && *(fmt + 1) == 'l') {
            length = LEN_LL;
            fmt += 2;
        } else if (*fmt == 'l') {
            length = LEN_L;
            fmt++;
        } else if (*fmt == 'z') {
            length = LEN_Z;
            fmt++;
        } else if (*fmt == 't') {
            length = LEN_T;
            fmt++;
        } else if (*fmt == 'j') {
            length = LEN_J;
            fmt++;
        }

        char spec = *fmt ? *fmt++ : '\0';
        if (spec == '\0') {
            break;
        }

        if (spec == '%') {
            mirrored_putc('%');
            count++;
            continue;
        }

        if (spec == 'c') {
            char c = (char)va_arg(args, int);
            int pad = width > 1 ? width - 1 : 0;
            if (!left) {
                count += serial_write_repeat(' ', pad);
            }
            mirrored_putc(c);
            count++;
            if (left) {
                count += serial_write_repeat(' ', pad);
            }
            continue;
        }

        if (spec == 's') {
            const char *s = va_arg(args, const char *);
            if (!s) {
                s = "(null)";
            }
            size_t len = 0;
            while (s[len] && (precision < 0 || (int)len < precision)) {
                len++;
            }
            int pad = width > (int)len ? width - (int)len : 0;
            if (!left) {
                count += serial_write_repeat(' ', pad);
            }
            count += serial_write_n(s, len);
            if (left) {
                count += serial_write_repeat(' ', pad);
            }
            continue;
        }

        if (spec == 'n') {
            void *ptr = va_arg(args, void *);
            if (ptr) {
                switch (length) {
                    case LEN_HH: *(signed char *)ptr = (signed char)count; break;
                    case LEN_H:  *(short *)ptr = (short)count; break;
                    case LEN_L:  *(long *)ptr = (long)count; break;
                    case LEN_LL: *(long long *)ptr = (long long)count; break;
                    case LEN_Z:  *(size_t *)ptr = (size_t)count; break;
                    case LEN_T:  *(ptrdiff_t *)ptr = (ptrdiff_t)count; break;
                    case LEN_J:  *(intmax_t *)ptr = (intmax_t)count; break;
                    default:     *(int *)ptr = count; break;
                }
            }
            continue;
        }

        int is_signed = (spec == 'd' || spec == 'i');
        int base = 10;
        int uppercase = 0;
        if (spec == 'x' || spec == 'X' || spec == 'p') {
            base = 16;
            uppercase = (spec == 'X');
        } else if (spec == 'o') {
            base = 8;
        } else if (spec == 'u') {
            base = 10;
        } else if (!is_signed) {
            continue;
        }

        uintmax_t uvalue = 0;
        int negative = 0;
        if (spec == 'p') {
            uintptr_t p = (uintptr_t)va_arg(args, void *);
            uvalue = (uintmax_t)p;
        } else if (is_signed) {
            intmax_t svalue = 0;
            switch (length) {
                case LEN_HH: svalue = (signed char)va_arg(args, int); break;
                case LEN_H:  svalue = (short)va_arg(args, int); break;
                case LEN_L:  svalue = va_arg(args, long); break;
                case LEN_LL: svalue = va_arg(args, long long); break;
                case LEN_Z:  svalue = (ptrdiff_t)va_arg(args, ptrdiff_t); break;
                case LEN_T:  svalue = va_arg(args, ptrdiff_t); break;
                case LEN_J:  svalue = va_arg(args, intmax_t); break;
                default:     svalue = va_arg(args, int); break;
            }
            if (svalue < 0) {
                negative = 1;
                uvalue = (uintmax_t)(-svalue);
            } else {
                uvalue = (uintmax_t)svalue;
            }
        } else {
            switch (length) {
                case LEN_HH: uvalue = (unsigned char)va_arg(args, unsigned int); break;
                case LEN_H:  uvalue = (unsigned short)va_arg(args, unsigned int); break;
                case LEN_L:  uvalue = va_arg(args, unsigned long); break;
                case LEN_LL: uvalue = va_arg(args, unsigned long long); break;
                case LEN_Z:  uvalue = va_arg(args, size_t); break;
                case LEN_T:  uvalue = (ptrdiff_t)va_arg(args, ptrdiff_t); break;
                case LEN_J:  uvalue = va_arg(args, uintmax_t); break;
                default:     uvalue = va_arg(args, unsigned int); break;
            }
        }

        char num_buf[64];
        int num_len = 0;
        if (precision == 0 && uvalue == 0 && spec != 'p') {
            num_buf[0] = '\0';
            num_len = 0;
        } else {
            num_len = utoa_base(uvalue, (unsigned)base, uppercase, num_buf, (int)sizeof(num_buf));
        }

        char sign_char = '\0';
        if (is_signed) {
            if (negative) {
                sign_char = '-';
            } else if (plus) {
                sign_char = '+';
            } else if (space) {
                sign_char = ' ';
            }
        }

        char prefix[2];
        int prefix_len = 0;
        if (spec == 'p') {
            prefix[0] = '0';
            prefix[1] = 'x';
            prefix_len = 2;
        } else if (alt) {
            if ((spec == 'x' || spec == 'X') && uvalue != 0) {
                prefix[0] = '0';
                prefix[1] = uppercase ? 'X' : 'x';
                prefix_len = 2;
            } else if (spec == 'o') {
                if (uvalue != 0 || precision == 0) {
                    prefix[0] = '0';
                    prefix_len = 1;
                }
            }
        }

        int zero_pad = 0;
        if (precision >= 0) {
            zero_pad = max_int(0, precision - num_len);
        }

        int total_len = num_len + zero_pad + prefix_len + (sign_char ? 1 : 0);
        int pad_len = width > total_len ? width - total_len : 0;

        int zero_width_pad = 0;
        if (zero && !left && precision < 0 && (spec == 'p' || is_signed || spec == 'u' || spec == 'x' || spec == 'X' || spec == 'o')) {
            zero_width_pad = pad_len;
            pad_len = 0;
        }

        if (!left) {
            count += serial_write_repeat(' ', pad_len);
        }

        if (sign_char) {
            mirrored_putc(sign_char);
            count++;
        }
        if (prefix_len) {
            count += serial_write_n(prefix, (size_t)prefix_len);
        }
        if (zero_width_pad) {
            count += serial_write_repeat('0', zero_width_pad);
        }
        count += serial_write_repeat('0', zero_pad);
        if (num_len > 0) {
            count += serial_write_n(num_buf, (size_t)num_len);
        }

        if (left) {
            count += serial_write_repeat(' ', pad_len);
        }
    }

    va_end(args);
    return count;
}