#include "net.h"

#include "nic/nic.h"
#include "link/ethernet.h"
#include "internet/icmp.h"
#include "core/serial.h"

static net_rx_handler_t g_net_rx_handler = NULL;
static uint32_t g_our_ip = 0x0A00020FU; // 10.0.2.15

static void net_rx_bridge(const uint8_t *frame, uint16_t len) {
	if (g_net_rx_handler) {
		g_net_rx_handler(frame, len);
	}
	eth_input(frame, len);
}

int net_init(void) {
	rtl8139_set_rx_callback(net_rx_bridge);
	return rtl8139_init();
}

uint32_t net_ip(void) {
	return g_our_ip;
}

void net_set_ip(uint32_t ip_host) {
	g_our_ip = ip_host;
}

void net_ping(uint32_t dst_ip, uint16_t seq) {
	icmp_send_echo(dst_ip, 0x0001, seq);
}

void net_poll(void) {
	rtl8139_poll();
}

int net_send(const void *frame, uint16_t len) {
	return rtl8139_send(frame, len);
}

void net_set_rx_handler(net_rx_handler_t handler) {
	g_net_rx_handler = handler;
}

const uint8_t *net_mac_address(void) {
	return rtl8139_mac_address();
}

int net_link_is_up(void) {
	return rtl8139_link_is_up();
}

const struct rtl8139_stats *net_stats(void) {
	return rtl8139_get_stats();
}
