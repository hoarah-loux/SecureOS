#ifndef SECUREOS_NETWORK_NET_H
#define SECUREOS_NETWORK_NET_H

#include <stdint.h>

struct rtl8139_stats;

typedef void (*net_rx_handler_t)(const uint8_t *frame, uint16_t len);

int net_init(void);
void net_poll(void);
int net_send(const void *frame, uint16_t len);
void net_set_rx_handler(net_rx_handler_t handler);
const uint8_t *net_mac_address(void);
int net_link_is_up(void);
const struct rtl8139_stats *net_stats(void);

// Our IPv4 address in host byte order (default 10.0.2.15 for QEMU user-net).
uint32_t net_ip(void);
void net_set_ip(uint32_t ip_host);

// Send an ICMP echo request to dst_ip (host byte order).
// Returns 0 immediately; the ARP resolution and reply are handled inside
// the polling loop via net_poll().
void net_ping(uint32_t dst_ip, uint16_t seq);

#endif
