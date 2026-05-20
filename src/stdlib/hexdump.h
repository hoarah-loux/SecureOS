#include <stdint.h>
#include <stddef.h>

// include serial to print
#include "core/serial.h"

#ifndef HEXDUMP_H
#define HEXDUMP_H

size_t hexdump(const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t offset = 0; offset < size; offset += 16) {
        serial_printf("%08x  ", (uint32_t)offset);

        for (size_t i = 0; i < 16; i++) {
            if (offset + i < size) {
                serial_printf("%02x ", bytes[offset + i]);
            } else {
                serial_write_string("   ");
            }
            if (i == 7) {
                serial_write_char(' ');
            }
        }

        serial_write_string(" |");
        for (size_t i = 0; i < 16 && offset + i < size; i++) {
            uint8_t c = bytes[offset + i];
            if (c >= 0x20 && c <= 0x7E) {
                serial_write_char((char)c);
            } else {
                serial_write_char('.');
            }
        }
        serial_write_string("|\n");
    }
    return 0;
}



#endif /* HEXDUMP_H */