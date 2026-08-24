// Out-of-line definitions of the mem* routines.
//
// hdr/string.h already provides `static inline` versions, which cover every
// *explicit* call site. They do not cover the calls GCC generates on its own:
// a struct assignment (e.g. `mount->boot_sector = *(const fat32_boot_sector_t *)buf;`
// in fat.c) is lowered to a call to the external symbol `memcpy`, and a large
// zero-initializer is lowered to `memset`. Those never bind to a file-local
// static, so the link fails with an undefined reference unless a real global
// symbol exists somewhere in the image. This file provides it.
//
// Deliberately does NOT include hdr/string.h: that header's static definitions
// would shadow these and defeat the purpose.

#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    for (size_t i = 0; i < n; i++) {
        p[i] = (unsigned char)c;
    }
    return s;
}

// Overlap-safe copy. GCC emits this for overlapping struct/array moves.
void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0) {
        return dest;
    }

    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return (int)a[i] - (int)b[i];
        }
    }
    return 0;
}
