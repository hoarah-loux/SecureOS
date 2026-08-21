#include "checksum.h"

static uint32_t sum16(const uint8_t *p, size_t len, uint32_t acc) {
    while (len > 1) {
        acc += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len == 1) {
        acc += (uint32_t)(p[0] << 8);
    }
    return acc;
}

static uint16_t fold(uint32_t acc) {
    while (acc >> 16) {
        acc = (acc & 0xFFFF) + (acc >> 16);
    }
    return (uint16_t)~acc;
}

uint16_t inet_checksum(const void *data, size_t len) {
    uint32_t acc = sum16((const uint8_t *)data, len, 0);
    // sum16 accumulates big-endian: p[0] is the high byte of each word.
    // fold() therefore produces a big-endian result where the high byte
    // (csum >> 8) is the first network byte and (csum & 0xFF) is the second.
    // Callers must store as: buf[n] = csum >> 8;  buf[n+1] = csum & 0xFF;
    return fold(acc);
}

uint16_t inet_checksum_pseudo(uint32_t src_ip_net, uint32_t dst_ip_net,
                              uint8_t proto, const void *data, size_t len) {
    uint8_t ph[12];
    const uint8_t *s = (const uint8_t *)&src_ip_net;
    const uint8_t *d = (const uint8_t *)&dst_ip_net;

    ph[0] = s[0]; ph[1] = s[1]; ph[2] = s[2]; ph[3] = s[3];
    ph[4] = d[0]; ph[5] = d[1]; ph[6] = d[2]; ph[7] = d[3];
    ph[8] = 0;
    ph[9] = proto;
    ph[10] = (uint8_t)((len >> 8) & 0xFF);
    ph[11] = (uint8_t)(len & 0xFF);

    uint32_t acc = sum16(ph, sizeof(ph), 0);
    acc = sum16((const uint8_t *)data, len, acc);
    return fold(acc);
}
