#include "ethernet.h"
#include "arp.h"
#include "../internet/ipv4.h"
#include "../net.h"
#include "../nic/nic.h"
#include "../util/endian.h"
#include "core/serial.h"

#define ETH_MTU_PAYLOAD 1500

static uint8_t g_tx_frame[ETH_HDR_LEN + ETH_MTU_PAYLOAD];

static void copy(uint8_t *dst, const uint8_t *src, uint16_t n) {
    for (uint16_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

void eth_input(const uint8_t *frame, uint16_t len) {
    if (frame == NULL || len < ETH_HDR_LEN) {
        return;
    }

    uint16_t ethertype = (uint16_t)((frame[12] << 8) | frame[13]);
    const uint8_t *payload = frame + ETH_HDR_LEN;
    uint16_t payload_len = (uint16_t)(len - ETH_HDR_LEN);

    switch (ethertype) {
        case ETH_TYPE_ARP:
            arp_input(payload, payload_len);
            break;
        case ETH_TYPE_IPV4:
            ipv4_input(frame, payload, payload_len);
            break;
        default:
            break;
    }
}

int eth_output(const uint8_t dst_mac[ETH_ALEN], uint16_t ethertype,
               const void *payload, uint16_t len) {
    if (len > ETH_MTU_PAYLOAD) {
        return -1;
    }

    const uint8_t *src_mac = net_mac_address();

    copy(&g_tx_frame[0], dst_mac, ETH_ALEN);
    copy(&g_tx_frame[6], src_mac, ETH_ALEN);
    g_tx_frame[12] = (uint8_t)(ethertype >> 8);
    g_tx_frame[13] = (uint8_t)(ethertype & 0xFF);

    if (payload && len) {
        copy(&g_tx_frame[ETH_HDR_LEN], (const uint8_t *)payload, len);
    }

    return net_send(g_tx_frame, (uint16_t)(ETH_HDR_LEN + len));
}
