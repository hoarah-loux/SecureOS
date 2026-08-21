#ifndef SECUREOS_NETWORK_ETHERNET_H
#define SECUREOS_NETWORK_ETHERNET_H

#include <stddef.h>
#include <stdint.h>

#define ETH_ALEN          6
#define ETH_HDR_LEN       14
#define ETH_TYPE_IPV4     0x0800
#define ETH_TYPE_ARP      0x0806

struct eth_addr {
    uint8_t addr[ETH_ALEN];
};

// Called by the NIC layer for every received frame.
void eth_input(const uint8_t *frame, uint16_t len);

// Build an Ethernet frame around payload and transmit it via the NIC.
// dst_mac is 6 bytes; ethertype is in host order. Returns 0 on success.
int eth_output(const uint8_t dst_mac[ETH_ALEN], uint16_t ethertype,
               const void *payload, uint16_t len);

#endif
