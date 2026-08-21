#include "ipv4.h"
#include "icmp.h"
#include "../transport/udp.h"
#include "../link/ethernet.h"
#include "../link/arp.h"
#include "../net.h"
#include "../util/checksum.h"
#include "core/serial.h"

#define IP_MTU_PAYLOAD 1480

static uint8_t g_ip_tx[IPV4_HDR_LEN + IP_MTU_PAYLOAD];
static uint16_t g_ip_id = 0;

static uint32_t load_ip(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_ip(uint8_t *p, uint32_t ip) {
    p[0] = (uint8_t)(ip >> 24);
    p[1] = (uint8_t)(ip >> 16);
    p[2] = (uint8_t)(ip >> 8);
    p[3] = (uint8_t)ip;
}

void ipv4_input(const uint8_t *frame, const uint8_t *payload, uint16_t len) {
    if (frame == NULL || payload == NULL || len < IPV4_HDR_LEN) {
        return;
    }

    uint8_t version = payload[0] >> 4;
    uint8_t ihl = (uint8_t)((payload[0] & 0x0F) * 4);
    if (version != 4 || ihl < IPV4_HDR_LEN || ihl > len) {
        return;
    }

    uint16_t total_len = (uint16_t)((payload[2] << 8) | payload[3]);
    if (total_len > len || total_len < ihl) {
        return;
    }

    uint8_t proto = payload[9];
    uint32_t src_ip = load_ip(&payload[12]);
    uint32_t dst_ip = load_ip(&payload[16]);

    if (dst_ip != net_ip() && dst_ip != 0xFFFFFFFFU) {
        return;
    }

    // Learn the sender so replies don't need a fresh ARP round-trip.
    arp_cache_put(src_ip, &frame[6]);

    const uint8_t *l4 = payload + ihl;
    uint16_t l4_len = (uint16_t)(total_len - ihl);

    switch (proto) {
        case IPPROTO_ICMP:
            icmp_input(src_ip, l4, l4_len);
            break;
        case IPPROTO_UDP:
            udp_input(src_ip, l4, l4_len);
            break;
        default:
            break;
    }
}

int ipv4_output(uint32_t dst_ip, uint8_t proto, const void *payload, uint16_t len) {
    if (len > IP_MTU_PAYLOAD) {
        return -1;
    }

    uint8_t dst_mac[6];
    if (!arp_lookup(dst_ip, dst_mac)) {
        arp_request(dst_ip);
        return -2;
    }

    uint16_t total = (uint16_t)(IPV4_HDR_LEN + len);

    g_ip_tx[0] = 0x45;
    g_ip_tx[1] = 0x00;
    g_ip_tx[2] = (uint8_t)(total >> 8);
    g_ip_tx[3] = (uint8_t)total;
    g_ip_tx[4] = (uint8_t)(g_ip_id >> 8);
    g_ip_tx[5] = (uint8_t)g_ip_id;
    g_ip_id++;
    g_ip_tx[6] = 0x00;
    g_ip_tx[7] = 0x00;
    g_ip_tx[8] = 64;
    g_ip_tx[9] = proto;
    g_ip_tx[10] = 0x00;
    g_ip_tx[11] = 0x00;
    store_ip(&g_ip_tx[12], net_ip());
    store_ip(&g_ip_tx[16], dst_ip);

    uint16_t csum = inet_checksum(g_ip_tx, IPV4_HDR_LEN);
    g_ip_tx[10] = (uint8_t)(csum >> 8);    /* first  network byte */
    g_ip_tx[11] = (uint8_t)(csum & 0xFF);  /* second network byte */

    const uint8_t *src = (const uint8_t *)payload;
    for (uint16_t i = 0; i < len; i++) {
        g_ip_tx[IPV4_HDR_LEN + i] = src[i];
    }

    return eth_output(dst_mac, ETH_TYPE_IPV4, g_ip_tx, total);
}
