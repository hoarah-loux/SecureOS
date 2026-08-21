#ifndef SECUREOS_NETWORK_ARP_H
#define SECUREOS_NETWORK_ARP_H

#include <stddef.h>
#include <stdint.h>

#define ARP_HW_ETHER   1
#define ARP_PROTO_IPV4 0x0800
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

// Handle an incoming ARP payload (after the Ethernet header).
void arp_input(const uint8_t *payload, uint16_t len);

// Broadcast an ARP request for the given IPv4 address (host byte order).
void arp_request(uint32_t target_ip);

// Look up a cached MAC for an IPv4 address (host order). Returns 1 and fills
// mac on hit, 0 on miss.
int arp_lookup(uint32_t ip, uint8_t mac[6]);

// Insert/update a cache entry.
void arp_cache_put(uint32_t ip, const uint8_t mac[6]);

#endif
