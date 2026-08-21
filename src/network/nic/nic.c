#include <stddef.h>
#include <stdint.h>

#include "nic.h"
#include "core/serial.h"

// PCI config I/O ports and RTL8139 register offsets are intentionally defined
// in this network driver to keep hardware addresses scoped to network code.

#define PCI_CONFIG_ADDRESS_PORT 0xCF8
#define PCI_CONFIG_DATA_PORT    0xCFC

#define RTL8139_VENDOR_ID 0x10EC
#define RTL8139_DEVICE_ID 0x8139

#define PCI_COMMAND_REG        0x04
#define PCI_BAR0_REG           0x10
#define PCI_INTERRUPT_LINE_REG 0x3C

enum rtl8139_registers {
	RTL_MAC0       = 0x00,
	RTL_MAR0       = 0x08,
	RTL_TX_STATUS0 = 0x10,
	RTL_TX_ADDR0   = 0x20,
	RTL_RX_BUF     = 0x30,
	RTL_CHIP_CMD   = 0x37,
	RTL_RX_BUF_PTR = 0x38,
	RTL_INTR_MASK  = 0x3C,
	RTL_INTR_STAT  = 0x3E,
	RTL_TX_CONFIG  = 0x40,
	RTL_RX_CONFIG  = 0x44,
	RTL_RX_MISSED  = 0x4C,
	RTL_CFG9346    = 0x50,
	RTL_CONFIG1    = 0x52,
	RTL_MII_BMSR   = 0x64,
	RTL_CSCR       = 0x74,
};

enum rtl8139_chip_cmd_bits {
	RTL_RX_BUF_EMPTY = 0x01,
	RTL_CMD_TX_ENB   = 0x04,
	RTL_CMD_RX_ENB   = 0x08,
	RTL_CMD_RESET    = 0x10,
};

enum rtl8139_intr_bits {
	RTL_INT_RX_OK       = 0x0001,
	RTL_INT_RX_ERR      = 0x0002,
	RTL_INT_TX_OK       = 0x0004,
	RTL_INT_TX_ERR      = 0x0008,
	RTL_INT_RX_OVERFLOW = 0x0010,
	RTL_INT_RX_UNDERRUN = 0x0020,
	RTL_INT_RX_FIFO_OVR = 0x0040,
	RTL_INT_PCS_TIMEOUT = 0x4000,
	RTL_INT_PCI_ERR     = 0x8000,
};

enum rtl8139_rx_status_bits {
	RTL_RX_STATUS_OK = 0x0001,
	RTL_RX_BAD_ALIGN = 0x0002,
	RTL_RX_CRC_ERR   = 0x0004,
	RTL_RX_TOO_LONG  = 0x0008,
	RTL_RX_RUNT      = 0x0010,
	RTL_RX_BAD_SYM   = 0x0020,
};

enum rtl8139_tx_status_bits {
	RTL_TX_UNDERRUN      = 0x00004000,
	RTL_TX_STAT_OK       = 0x00008000,
	RTL_TX_OUT_OF_WINDOW = 0x20000000,
	RTL_TX_ABORTED       = 0x40000000,
	RTL_TX_CARRIER_LOST  = 0x80000000,
};

enum rtl8139_rx_config_bits {
	RTL_ACCEPT_ALL_PHYS   = 0x01,
	RTL_ACCEPT_MY_PHYS    = 0x02,
	RTL_ACCEPT_MULTICAST  = 0x04,
	RTL_ACCEPT_BROADCAST  = 0x08,
	RTL_ACCEPT_RUNT       = 0x10,
	RTL_ACCEPT_ERR        = 0x20,
};

#define RTL_RX_BUF_LEN_IDX 0U
#define RTL_RX_BUF_LEN     8192U
#define RTL_RX_BUF_PAD     (16U + 1500U)
#define RTL_RX_BUF_TOTAL   (RTL_RX_BUF_LEN + RTL_RX_BUF_PAD)

#define RTL_TX_DESC_COUNT 4U
#define RTL_TX_BUF_SIZE   1536U

#define RTL_RX_FIFO_THRESH 4U
#define RTL_RX_DMA_BURST   4U
#define RTL_TX_DMA_BURST   4U
#define RTL_TX_FIFO_THRESH 256U

#define RTL_LINK_OK_BIT 0x0400

struct rtl8139_pci_id {
	uint8_t bus;
	uint8_t slot;
	uint8_t func;
};

struct rtl8139_device {
	int present;
	uint16_t io_base;
	uint8_t irq_line;
	struct rtl8139_pci_id pci;
	uint8_t mac[RTL8139_ETH_ADDR_LEN];

	uint32_t cur_rx;
	uint32_t tx_producer;
	uint32_t tx_consumer;

	rtl8139_rx_callback_t rx_callback;
	struct rtl8139_stats stats;
};

static struct rtl8139_device g_rtl8139;

static uint8_t g_rx_ring[RTL_RX_BUF_TOTAL] __attribute__((aligned(16)));
static uint8_t g_tx_buffer[RTL_TX_DESC_COUNT][RTL_TX_BUF_SIZE] __attribute__((aligned(16)));
static uint8_t g_rx_scratch[RTL_TX_BUF_SIZE] __attribute__((aligned(16)));

static inline void outb(uint16_t port, uint8_t value) {
	__asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value) {
	__asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t value) {
	__asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
	uint8_t value;
	__asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static inline uint16_t inw(uint16_t port) {
	uint16_t value;
	__asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static inline uint32_t inl(uint16_t port) {
	uint32_t value;
	__asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static void mem_copy(void *dst, const void *src, size_t len) {
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	for (size_t i = 0; i < len; i++) {
		d[i] = s[i];
	}
}

static void mem_set(void *dst, uint8_t value, size_t len) {
	uint8_t *d = (uint8_t *)dst;

	for (size_t i = 0; i < len; i++) {
		d[i] = value;
	}
}

static uint32_t load_le32(const uint8_t *p) {
	return ((uint32_t)p[0]) |
		   ((uint32_t)p[1] << 8) |
		   ((uint32_t)p[2] << 16) |
		   ((uint32_t)p[3] << 24);
}

static uint32_t virt_to_phys32(const void *addr) {
	return (uint32_t)(uintptr_t)addr;
}

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
	uint32_t address = 0x80000000U |
					   ((uint32_t)bus << 16) |
					   ((uint32_t)slot << 11) |
					   ((uint32_t)func << 8) |
					   ((uint32_t)offset & 0xFCU);

	outl(PCI_CONFIG_ADDRESS_PORT, address);
	return inl(PCI_CONFIG_DATA_PORT);
}

static void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
	uint32_t address = 0x80000000U |
					   ((uint32_t)bus << 16) |
					   ((uint32_t)slot << 11) |
					   ((uint32_t)func << 8) |
					   ((uint32_t)offset & 0xFCU);

	outl(PCI_CONFIG_ADDRESS_PORT, address);
	outl(PCI_CONFIG_DATA_PORT, value);
}

static uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
	uint32_t value = pci_read32(bus, slot, func, (uint8_t)(offset & 0xFCU));
	uint8_t shift = (uint8_t)((offset & 2U) * 8U);
	return (uint16_t)((value >> shift) & 0xFFFFU);
}

static uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
	uint32_t value = pci_read32(bus, slot, func, (uint8_t)(offset & 0xFCU));
	uint8_t shift = (uint8_t)((offset & 3U) * 8U);
	return (uint8_t)((value >> shift) & 0xFFU);
}

static void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t reg_value) {
	uint8_t aligned = (uint8_t)(offset & 0xFCU);
	uint32_t old_value = pci_read32(bus, slot, func, aligned);
	uint8_t shift = (uint8_t)((offset & 2U) * 8U);
	uint32_t mask = 0xFFFFU << shift;
	uint32_t new_value = (old_value & ~mask) | ((uint32_t)reg_value << shift);

	pci_write32(bus, slot, func, aligned, new_value);
}

static int rtl8139_pci_probe(void) {
	for (uint16_t bus = 0; bus < 256; bus++) {
		for (uint8_t slot = 0; slot < 32; slot++) {
			uint16_t vendor = pci_read16((uint8_t)bus, slot, 0, 0x00);
			if (vendor == 0xFFFFU) {
				continue;
			}

			uint8_t header_type = pci_read8((uint8_t)bus, slot, 0, 0x0E);
			uint8_t function_count = (header_type & 0x80U) ? 8U : 1U;

			for (uint8_t func = 0; func < function_count; func++) {
				vendor = pci_read16((uint8_t)bus, slot, func, 0x00);
				if (vendor == 0xFFFFU) {
					continue;
				}

				uint16_t device = pci_read16((uint8_t)bus, slot, func, 0x02);
				if (vendor != RTL8139_VENDOR_ID || device != RTL8139_DEVICE_ID) {
					continue;
				}

				uint16_t command = pci_read16((uint8_t)bus, slot, func, PCI_COMMAND_REG);
				command |= 0x0005U;
				pci_write16((uint8_t)bus, slot, func, PCI_COMMAND_REG, command);

				uint32_t bar0 = pci_read32((uint8_t)bus, slot, func, PCI_BAR0_REG);
				if ((bar0 & 0x1U) == 0) {
					serial_write_string("[rtl8139] BAR0 is not I/O mapped\n");
					return -1;
				}

				g_rtl8139.io_base = (uint16_t)(bar0 & ~0x3U);
				g_rtl8139.irq_line = pci_read8((uint8_t)bus, slot, func, PCI_INTERRUPT_LINE_REG);
				g_rtl8139.pci.bus = (uint8_t)bus;
				g_rtl8139.pci.slot = slot;
				g_rtl8139.pci.func = func;

				return 0;
			}
		}
	}

	return -1;
}

static int rtl8139_reset(void) {
	outb((uint16_t)(g_rtl8139.io_base + RTL_CHIP_CMD), RTL_CMD_RESET);

	for (int i = 0; i < 100000; i++) {
		if ((inb((uint16_t)(g_rtl8139.io_base + RTL_CHIP_CMD)) & RTL_CMD_RESET) == 0) {
			return 0;
		}
	}

	return -1;
}

static void rtl8139_read_mac(void) {
	for (int i = 0; i < RTL8139_ETH_ADDR_LEN; i++) {
		g_rtl8139.mac[i] = inb((uint16_t)(g_rtl8139.io_base + RTL_MAC0 + i));
	}
}

static int rtl8139_hw_init(void) {
	if (rtl8139_reset() < 0) {
		serial_write_string("[rtl8139] reset timed out\n");
		return -1;
	}

	outb((uint16_t)(g_rtl8139.io_base + RTL_CFG9346), 0xC0);
	outb((uint16_t)(g_rtl8139.io_base + RTL_CONFIG1), 0x00);
	outb((uint16_t)(g_rtl8139.io_base + RTL_CFG9346), 0x00);

	g_rtl8139.cur_rx = 0;
	g_rtl8139.tx_producer = 0;
	g_rtl8139.tx_consumer = 0;

	mem_set(g_rx_ring, 0, sizeof(g_rx_ring));

	outl((uint16_t)(g_rtl8139.io_base + RTL_RX_BUF), virt_to_phys32(g_rx_ring));
	outl((uint16_t)(g_rtl8139.io_base + RTL_MAR0 + 0), 0xFFFFFFFFU);
	outl((uint16_t)(g_rtl8139.io_base + RTL_MAR0 + 4), 0xFFFFFFFFU);

	for (uint32_t i = 0; i < RTL_TX_DESC_COUNT; i++) {
		outl((uint16_t)(g_rtl8139.io_base + RTL_TX_ADDR0 + (i * 4U)), virt_to_phys32(g_tx_buffer[i]));
	}

	outl((uint16_t)(g_rtl8139.io_base + RTL_RX_MISSED), 0);

	uint32_t rx_config =
		(RTL_RX_FIFO_THRESH << 13) |
		(RTL_RX_BUF_LEN_IDX << 11) |
		(RTL_RX_DMA_BURST << 8) |
		RTL_ACCEPT_BROADCAST |
		RTL_ACCEPT_MULTICAST |
		RTL_ACCEPT_MY_PHYS;

	outl((uint16_t)(g_rtl8139.io_base + RTL_RX_CONFIG), rx_config);
	outl((uint16_t)(g_rtl8139.io_base + RTL_TX_CONFIG), (RTL_TX_DMA_BURST << 8));

	outb((uint16_t)(g_rtl8139.io_base + RTL_CHIP_CMD), RTL_CMD_RX_ENB | RTL_CMD_TX_ENB);

	outw((uint16_t)(g_rtl8139.io_base + RTL_INTR_STAT), 0xFFFFU);
	outw((uint16_t)(g_rtl8139.io_base + RTL_INTR_MASK),
		 RTL_INT_RX_OK |
		 RTL_INT_RX_ERR |
		 RTL_INT_TX_OK |
		 RTL_INT_TX_ERR |
		 RTL_INT_RX_OVERFLOW |
		 RTL_INT_RX_FIFO_OVR |
		 RTL_INT_RX_UNDERRUN |
		 RTL_INT_PCI_ERR |
		 RTL_INT_PCS_TIMEOUT);

	rtl8139_read_mac();
	return 0;
}

static void rtl8139_reap_tx(void) {
	while (g_rtl8139.tx_consumer != g_rtl8139.tx_producer) {
		uint32_t entry = g_rtl8139.tx_consumer % RTL_TX_DESC_COUNT;
		uint32_t status = inl((uint16_t)(g_rtl8139.io_base + RTL_TX_STATUS0 + (entry * 4U)));

		if ((status & (RTL_TX_STAT_OK | RTL_TX_UNDERRUN | RTL_TX_ABORTED)) == 0) {
			break;
		}

		if (status & (RTL_TX_ABORTED | RTL_TX_OUT_OF_WINDOW | RTL_TX_CARRIER_LOST)) {
			g_rtl8139.stats.tx_errors++;
		} else {
			g_rtl8139.stats.tx_packets++;
			g_rtl8139.stats.tx_bytes += (uint64_t)(status & 0x1FFFU);
			if (status & RTL_TX_UNDERRUN) {
				g_rtl8139.stats.tx_errors++;
			}
		}

		g_rtl8139.tx_consumer++;
	}
}

static void rtl8139_process_rx(void) {
	while ((inb((uint16_t)(g_rtl8139.io_base + RTL_CHIP_CMD)) & RTL_RX_BUF_EMPTY) == 0) {
		uint32_t ring_offset = g_rtl8139.cur_rx % RTL_RX_BUF_LEN;
		uint8_t *packet = &g_rx_ring[ring_offset];

		uint32_t rx_status = load_le32(packet);
		uint16_t status = (uint16_t)(rx_status & 0xFFFFU);
		uint16_t rx_size = (uint16_t)(rx_status >> 16);

		if ((status & RTL_RX_STATUS_OK) == 0 ||
			(status & (RTL_RX_BAD_SYM | RTL_RX_RUNT | RTL_RX_TOO_LONG | RTL_RX_CRC_ERR | RTL_RX_BAD_ALIGN)) != 0 ||
			rx_size < 4U ||
			rx_size > (RTL_RX_BUF_LEN + 4U)) {
			g_rtl8139.stats.rx_errors++;

			if (rtl8139_reset() < 0) {
				break;
			}

			outb((uint16_t)(g_rtl8139.io_base + RTL_CHIP_CMD), RTL_CMD_RX_ENB | RTL_CMD_TX_ENB);
			g_rtl8139.cur_rx = 0;
			outw((uint16_t)(g_rtl8139.io_base + RTL_RX_BUF_PTR), 0);
			break;
		}

		uint16_t frame_len = (uint16_t)(rx_size - 4U);
		if (frame_len > RTL8139_MAX_FRAME_LEN) {
			g_rtl8139.stats.rx_dropped++;
		} else {
			uint32_t frame_start = ring_offset + 4U;
			const uint8_t *frame_ptr = NULL;

			if (frame_start + frame_len <= RTL_RX_BUF_TOTAL) {
				frame_ptr = &g_rx_ring[frame_start];
			} else {
				uint32_t first = RTL_RX_BUF_TOTAL - frame_start;
				uint32_t second = frame_len - first;
				mem_copy(g_rx_scratch, &g_rx_ring[frame_start], first);
				mem_copy(g_rx_scratch + first, &g_rx_ring[0], second);
				frame_ptr = g_rx_scratch;
			}

			if (g_rtl8139.rx_callback) {
				g_rtl8139.rx_callback(frame_ptr, frame_len);
			}

			g_rtl8139.stats.rx_packets++;
			g_rtl8139.stats.rx_bytes += frame_len;
		}

		g_rtl8139.cur_rx = (g_rtl8139.cur_rx + rx_size + 4U + 3U) & ~3U;
		outw((uint16_t)(g_rtl8139.io_base + RTL_RX_BUF_PTR), (uint16_t)(g_rtl8139.cur_rx - 16U));
	}
}

int rtl8139_init(void) {
	mem_set(&g_rtl8139, 0, sizeof(g_rtl8139));

	if ((uintptr_t)g_rx_ring > 0xFFFFFFFFULL) {
		serial_write_string("[rtl8139] rx ring above 4GB, cannot DMA\n");
		return -1;
	}

	for (uint32_t i = 0; i < RTL_TX_DESC_COUNT; i++) {
		if ((uintptr_t)g_tx_buffer[i] > 0xFFFFFFFFULL) {
			serial_write_string("[rtl8139] tx ring above 4GB, cannot DMA\n");
			return -1;
		}
	}

	if (rtl8139_pci_probe() < 0) {
		serial_write_string("[rtl8139] device not found\n");
		return -1;
	}

	if (rtl8139_hw_init() < 0) {
		return -1;
	}

	g_rtl8139.present = 1;

	serial_printf("[rtl8139] found at bus=%u slot=%u func=%u io=0x%X irq=%u\n",
				  (unsigned)g_rtl8139.pci.bus,
				  (unsigned)g_rtl8139.pci.slot,
				  (unsigned)g_rtl8139.pci.func,
				  (unsigned)g_rtl8139.io_base,
				  (unsigned)g_rtl8139.irq_line);

	serial_printf("[rtl8139] mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
				  (unsigned)g_rtl8139.mac[0],
				  (unsigned)g_rtl8139.mac[1],
				  (unsigned)g_rtl8139.mac[2],
				  (unsigned)g_rtl8139.mac[3],
				  (unsigned)g_rtl8139.mac[4],
				  (unsigned)g_rtl8139.mac[5]);

	return 0;
}

void rtl8139_poll(void) {
	if (!g_rtl8139.present) {
		return;
	}

	uint16_t status = inw((uint16_t)(g_rtl8139.io_base + RTL_INTR_STAT));

	if (status == 0xFFFFU) {
		return;
	}

	if (status != 0) {
		outw((uint16_t)(g_rtl8139.io_base + RTL_INTR_STAT), status);
	}

	if (status & (RTL_INT_RX_OK | RTL_INT_RX_ERR | RTL_INT_RX_OVERFLOW | RTL_INT_RX_FIFO_OVR | RTL_INT_RX_UNDERRUN)) {
		rtl8139_process_rx();
	}

	if (status & (RTL_INT_TX_OK | RTL_INT_TX_ERR)) {
		rtl8139_reap_tx();
	}

	if (status & (RTL_INT_PCI_ERR | RTL_INT_PCS_TIMEOUT)) {
		g_rtl8139.stats.rx_errors++;
	}

	if (status == 0) {
		rtl8139_process_rx();
		rtl8139_reap_tx();
	}
}

int rtl8139_send(const void *frame, uint16_t len) {
	if (!g_rtl8139.present || frame == NULL || len == 0) {
		return -1;
	}

	if (len > RTL8139_MAX_FRAME_LEN) {
		g_rtl8139.stats.tx_dropped++;
		return -1;
	}

	rtl8139_reap_tx();

	if ((g_rtl8139.tx_producer - g_rtl8139.tx_consumer) >= RTL_TX_DESC_COUNT) {
		g_rtl8139.stats.tx_dropped++;
		return -2;
	}

	uint32_t entry = g_rtl8139.tx_producer % RTL_TX_DESC_COUNT;
	uint32_t tx_len = (len < RTL8139_MIN_FRAME_LEN) ? RTL8139_MIN_FRAME_LEN : len;

	mem_copy(g_tx_buffer[entry], frame, len);
	if (tx_len > len) {
		mem_set(&g_tx_buffer[entry][len], 0, tx_len - len);
	}

	outl((uint16_t)(g_rtl8139.io_base + RTL_TX_ADDR0 + (entry * 4U)), virt_to_phys32(g_tx_buffer[entry]));
	outl((uint16_t)(g_rtl8139.io_base + RTL_TX_STATUS0 + (entry * 4U)),
		 ((RTL_TX_FIFO_THRESH << 11) & 0x003F0000U) | tx_len);

	g_rtl8139.tx_producer++;
	return 0;
}

void rtl8139_set_rx_callback(rtl8139_rx_callback_t callback) {
	g_rtl8139.rx_callback = callback;
}

const uint8_t *rtl8139_mac_address(void) {
	return g_rtl8139.mac;
}

int rtl8139_link_is_up(void) {
	if (!g_rtl8139.present) {
		return 0;
	}

	if ((inw((uint16_t)(g_rtl8139.io_base + RTL_CSCR)) & RTL_LINK_OK_BIT) != 0) {
		return 1;
	}

	return (inw((uint16_t)(g_rtl8139.io_base + RTL_MII_BMSR)) & 0x0004U) != 0;
}

const struct rtl8139_stats *rtl8139_get_stats(void) {
	return &g_rtl8139.stats;
}

int rtl8139_iobase(void) {
	return (int)g_rtl8139.io_base;
}

int rtl8139_irq_line(void) {
	return (int)g_rtl8139.irq_line;
}
