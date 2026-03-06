#include "core/serial.h"

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

void serial_write_char(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) {
        /* wait */
    }
    outb(COM1, (uint8_t)c);
}

void serial_write_string(const char *s) {
    while (*s) {
        serial_write_char(*s++);
    }
}

void serial_write_hex32(uint32_t value) {
    for (int i = 0; i < 8; i++) {
        uint8_t nibble = (value >> 28) & 0xF;
        char c = (nibble <= 9) ? (char)('0' + nibble) : (char)('A' + (nibble - 10));
        serial_write_char(c);
        value <<= 4;
    }
}

void serial_write_hex64(uint64_t value) {
    serial_write_hex32((uint32_t)(value >> 32));
    serial_write_hex32((uint32_t)(value & 0xFFFFFFFFu));
}
