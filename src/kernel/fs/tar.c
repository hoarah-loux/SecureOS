#include <tar.h>
#include <string.h>
#include <serial.h>

#define TAR_BLOCK_SIZE 512

static int is_zero_block(const uint8_t *p) {
    for (uint32_t i = 0; i < TAR_BLOCK_SIZE; i++) {
        if (p[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static int is_ustar_header(const uint8_t *p) {
    /* ustar magic at offset 257 ("ustar" + NUL) */
    return (p[257] == 'u' && p[258] == 's' && p[259] == 't' && p[260] == 'a' && p[261] == 'r');
}

static void dump_bytes(const uint8_t *p, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if ((i % 16) == 0) {
            serial_printf("\n%04X: ", i);
        }
        serial_printf("%02X ", p[i]);
    }
    serial_write_string("\r\n");
}

static void dump_text_bytes(const uint8_t *p, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint8_t c = p[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            serial_write_char((char)c);
        } else if (c >= 0x20 && c <= 0x7E) {
            serial_write_char((char)c);
        } else {
            serial_write_char('.');
        }
    }
    serial_write_string("\r\n");
}

uint32_t o2d(const char* octal) {
    uint32_t size = 0;
    uint32_t i;
    uint32_t count = 1;

    for (i = 11; i > 0; i--, count *= 8)
        size += (octal[i - 1] - '0') * count;

    return size;
}

uint32_t parse_headers(uintptr_t tar_start) {
    const uint8_t *base = (const uint8_t *)tar_start;
    uint32_t offset = 0;
    uint32_t file_count = 0;

    while (1) {
        const uint8_t *block = base + offset;
        tar_header_t *header = (tar_header_t *)block;

        if (is_zero_block(block)) {
            serial_printf("[TAR] end of archive at offset %u\n", offset);
            break;
        }

        if (!is_ustar_header(block)) {
            serial_printf("[TAR] invalid header at offset %u (missing ustar magic)\n", offset);
            dump_bytes(block, 64);
            break;
        }

        uint32_t file_size = o2d(header->size);
        uint32_t total_size = ((file_size + 511) / 512 + 1) * 512; // file size rounded up to next 512 byte block, plus header block

        serial_printf("[TAR] File: %s\n", header->filename);
        serial_printf("[TAR] Size: %u bytes\n", file_size);
        serial_printf("[TAR] Mode: %s\n", header->mode);
        serial_printf("[TAR] UID: %s\n", header->uid);
        serial_printf("[TAR] GID: %s\n", header->gid);
        serial_printf("[TAR] MTime: %s\n", header->mtime);
        serial_printf("[TAR] ChkSum: %s\n", header->chksum);
        serial_printf("[TAR] TypeFlag: %c\n", header->typeflag);
        serial_printf("[TAR] Offset: %u bytes\n", offset);
        serial_printf("[TAR] Next header starts at: 0x%08X\n", (uint32_t)(tar_start + offset + total_size));

        offset += total_size;
        file_count++;
    }

    serial_printf("[TAR] files parsed: %u\n", file_count);
    return offset;
}

int tar_print_file(uintptr_t tar_start, const char *name) {
    const uint8_t *base = (const uint8_t *)tar_start;
    uint32_t offset = 0;

    while (1) {
        const uint8_t *block = base + offset;
        tar_header_t *header = (tar_header_t *)block;

        if (is_zero_block(block)) {
            serial_printf("[TAR] file not found: %s\n", name);
            return 0;
        }

        if (!is_ustar_header(block)) {
            serial_printf("[TAR] invalid header at offset %u (missing ustar magic)\n", offset);
            dump_bytes(block, 64);
            return 0;
        }

        uint32_t file_size = o2d(header->size);
        uint32_t total_size = ((file_size + 511) / 512 + 1) * 512;

        if (!strcmp(header->filename, name)) {
            const uint8_t *data = block + TAR_BLOCK_SIZE;
            serial_printf("[TAR] reading %s (%u bytes)\n", header->filename, file_size);
            dump_text_bytes(data, file_size);
            return 1;
        }

        offset += total_size;
    }
}