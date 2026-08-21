#ifndef SECUREOS_NETWORK_UDP_H
#define SECUREOS_NETWORK_UDP_H

#include <stdint.h>

#define UDP_HDR_LEN 8

typedef void (*udp_handler_t)(uint32_t src_ip, uint16_t src_port,
                              const uint8_t *data, uint16_t len);

// Handle an incoming UDP datagram (called by IPv4). src_ip is host order.
void udp_input(uint32_t src_ip, const uint8_t *payload, uint16_t len);

// Send a UDP datagram. ports/ip in host byte order. Returns 0 on success.
int udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
             const void *data, uint16_t len);

// Register a single application handler (NULL clears).
void udp_set_handler(udp_handler_t handler);

#endif
