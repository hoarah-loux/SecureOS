#include "arp.h"
#include "ethernet.h"
#include "../net.h"
#include "../util/endian.h"
#include "core/serial.h"

#define ARP_CACHE_SIZE 16
#define ARP_PKT_LEN    28

struct arp_entry {
    uint32_t ip;
    uint8_t mac[6];
    uint8_t valid;
};

static struct arp_entry g_cache[ARP_CACHE_SIZE];

int arp_lookup(uint32_t ip, uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].valid && g_cache[i].ip == ip) {
            for (int j = 0; j < 6; j++) {
                mac[j] = g_cache[i].mac[j];
            }
            return 1;
        }
    }
    return 0;
}

void arp_cache_put(uint32_t ip, const uint8_t mac[6]) {
    int slot = -1;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].valid && g_cache[i].ip == ip) {
            slot = i;
            break;
        }
        if (slot < 0 && !g_cache[i].valid) {
            slot = i;
        }
    }
    if (slot < 0) {
        slot = 0;
    }
    g_cache[slot].ip = ip;
    for (int j = 0; j < 6; j++) {
        g_cache[slot].mac[j] = mac[j];
    }
    g_cache[slot].valid = 1;
}

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

static void send_arp(uint16_t op, uint32_t target_ip,
                     const uint8_t tha[6], const uint8_t eth_dst[6]) {
    uint8_t pkt[ARP_PKT_LEN];
    const uint8_t *my_mac = net_mac_address();
    uint32_t my_ip = net_ip();

    pkt[0] = 0; pkt[1] = ARP_HW_ETHER;
    pkt[2] = 0x08; pkt[3] = 0x00;
    pkt[4] = 6; pkt[5] = 4;
    pkt[6] = (uint8_t)(op >> 8); pkt[7] = (uint8_t)op;
    for (int i = 0; i < 6; i++) pkt[8 + i] = my_mac[i];
    store_ip(&pkt[14], my_ip);
    for (int i = 0; i < 6; i++) pkt[18 + i] = tha[i];
    store_ip(&pkt[24], target_ip);

    eth_output(eth_dst, ETH_TYPE_ARP, pkt, ARP_PKT_LEN);
}

void arp_request(uint32_t target_ip) {
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t zeros[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    /* THA is all-zeros for requests (RFC 826); Ethernet dst is broadcast. */
    send_arp(ARP_OP_REQUEST, target_ip, zeros, bcast);
}

void arp_input(const uint8_t *payload, uint16_t len) {
    if (payload == NULL || len < ARP_PKT_LEN) {
        return;
    }

    uint16_t oper = (uint16_t)((payload[6] << 8) | payload[7]);
    const uint8_t *sha = &payload[8];
    uint32_t spa = load_ip(&payload[14]);
    uint32_t tpa = load_ip(&payload[24]);

    arp_cache_put(spa, sha);

    if (oper == ARP_OP_REQUEST && tpa == net_ip()) {
        /* Reply: THA = requester's MAC, Ethernet dst = requester's MAC. */
        send_arp(ARP_OP_REPLY, spa, sha, sha);
    }
}
