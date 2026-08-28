#pragma once

#include <stdint.h>

void serial_init(void);
void serial_write_char(char c);
void serial_write_string(const char *s);
void serial_write_hex32(uint32_t value);
void serial_write_hex64(uint64_t value);
int printk(const char *fmt, ...);

// Optional per-character mirror for printk output. When set, every character
// printk emits also goes through this sink - kmain points it at the
// framebuffer console so boot logs are visible on machines with no serial
// port (the whole point on bare metal). Serial-only helpers
// (serial_write_char, serial_write_hex64, ...) and userland's sys_write are
// NOT mirrored: sys_write already writes the console itself, and mirroring
// it here would double every character on screen.
typedef void (*printk_mirror_fn)(char c);
void printk_set_mirror(printk_mirror_fn fn);