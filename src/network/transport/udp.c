#include "udp.h"
#include "../internet/ipv4.h"
#include "../net.h"
#include "../util/checksum.h"
#include "core/serial.h"

#define UDP_MAX_PAYLOAD 1472

static udp_handler_t g_handler = 0;
static uint8_t g_udp_tx[UDP_HDR_LEN + UDP_MAX_PAYLOAD];

void udp_set_handler(udp_handler_t handler) {
    g_handler = handler;
}

void udp_input(uint32_t src_ip, const uint8_t *payload, uint16_t len) {
    if (payload == NULL || len < UDP_HDR_LEN) {
        return;
    }

    uint16_t src_port = (uint16_t)((payload[0] << 8) | payload[1]);
    uint16_t udp_len = (uint16_t)((payload[4] << 8) | payload[5]);
    if (udp_len < UDP_HDR_LEN || udp_len > len) {
        return;
    }

    const uint8_t *data = payload + UDP_HDR_LEN;
    uint16_t data_len = (uint16_t)(udp_len - UDP_HDR_LEN);

    if (g_handler) {
        g_handler(src_ip, src_port, data, data_len);
    }
}

int udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
             const void *data, uint16_t len) {
    if (len > UDP_MAX_PAYLOAD) {
        return -1;
    }

    uint16_t total = (uint16_t)(UDP_HDR_LEN + len);

    g_udp_tx[0] = (uint8_t)(src_port >> 8);
    g_udp_tx[1] = (uint8_t)src_port;
    g_udp_tx[2] = (uint8_t)(dst_port >> 8);
    g_udp_tx[3] = (uint8_t)dst_port;
    g_udp_tx[4] = (uint8_t)(total >> 8);
    g_udp_tx[5] = (uint8_t)total;
    g_udp_tx[6] = 0;
    g_udp_tx[7] = 0;

    const uint8_t *src = (const uint8_t *)data;
    for (uint16_t i = 0; i < len; i++) {
        g_udp_tx[UDP_HDR_LEN + i] = src[i];
    }

    // UDP checksum is optional for IPv4; leave zero for simplicity.
    return ipv4_output(dst_ip, IPPROTO_UDP, g_udp_tx, total);
}
