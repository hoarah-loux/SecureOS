#ifndef SECUREOS_NETWORK_NIC_H
#define SECUREOS_NETWORK_NIC_H

#include <stddef.h>
#include <stdint.h>

#define RTL8139_ETH_ADDR_LEN 6
#define RTL8139_MIN_FRAME_LEN 60
#define RTL8139_MAX_FRAME_LEN 1518

struct rtl8139_stats {
	uint64_t rx_packets;
	uint64_t tx_packets;
	uint64_t rx_bytes;
	uint64_t tx_bytes;
	uint64_t rx_errors;
	uint64_t tx_errors;
	uint64_t rx_dropped;
	uint64_t tx_dropped;
};

typedef void (*rtl8139_rx_callback_t)(const uint8_t *frame, uint16_t len);

int rtl8139_init(void);
void rtl8139_poll(void);
int rtl8139_send(const void *frame, uint16_t len);
void rtl8139_set_rx_callback(rtl8139_rx_callback_t callback);
const uint8_t *rtl8139_mac_address(void);
int rtl8139_link_is_up(void);
const struct rtl8139_stats *rtl8139_get_stats(void);
int rtl8139_iobase(void);
int rtl8139_irq_line(void);

#endif
