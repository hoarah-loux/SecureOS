#ifndef SECUREOS_NETWORK_ENDIAN_H
#define SECUREOS_NETWORK_ENDIAN_H

#include <stdint.h>

// x86_64 is little-endian; network byte order is big-endian. These helpers
// convert 16/32-bit host values to/from network order.

static inline uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint32_t bswap32(uint32_t v) {
    return ((v & 0x000000FFU) << 24) |
           ((v & 0x0000FF00U) << 8) |
           ((v & 0x00FF0000U) >> 8) |
           ((v & 0xFF000000U) >> 24);
}

static inline uint16_t htons(uint16_t v) { return bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return bswap32(v); }

#endif
