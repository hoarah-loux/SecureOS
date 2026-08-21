#ifndef SECUREOS_NETWORK_ICMP_H
#define SECUREOS_NETWORK_ICMP_H

#include <stdint.h>

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

// Handle an ICMP message addressed to us. src_ip is host byte order.
void icmp_input(uint32_t src_ip, const uint8_t *payload, uint16_t len);

// Send an ICMP echo request (ping) to dst_ip (host byte order).
void icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq);

#endif
