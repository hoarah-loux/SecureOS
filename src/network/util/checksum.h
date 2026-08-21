#ifndef SECUREOS_NETWORK_CHECKSUM_H
#define SECUREOS_NETWORK_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

// Standard one's-complement Internet checksum (RFC 1071).
// Returns the 16-bit checksum in network byte order, ready to store directly.
uint16_t inet_checksum(const void *data, size_t len);

// Checksum with a UDP/TCP-style pseudo-header (IPv4). All addresses/proto/len
// are in network byte order except where noted. Used for UDP checksums.
uint16_t inet_checksum_pseudo(uint32_t src_ip_net, uint32_t dst_ip_net,
                              uint8_t proto, const void *data, size_t len);

#endif
