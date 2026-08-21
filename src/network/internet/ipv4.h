#ifndef SECUREOS_NETWORK_IPV4_H
#define SECUREOS_NETWORK_IPV4_H

#include <stddef.h>
#include <stdint.h>

#define IPPROTO_ICMP 1
#define IPPROTO_UDP  17
#define IPV4_HDR_LEN 20

// Handle an incoming IPv4 packet. `frame` is the full Ethernet frame (used to
// learn the sender MAC); `payload` points at the IPv4 header; len is its size.
void ipv4_input(const uint8_t *frame, const uint8_t *payload, uint16_t len);

// Build and send an IPv4 packet to dst_ip (host order) with the given protocol.
// Returns 0 on success, negative if the destination MAC is unknown (an ARP
// request is issued; retry after the reply arrives).
int ipv4_output(uint32_t dst_ip, uint8_t proto, const void *payload, uint16_t len);

#endif
