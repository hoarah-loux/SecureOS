#include "icmp.h"
#include "ipv4.h"
#include "../util/checksum.h"
#include "core/serial.h"

#define ICMP_MAX 1480

static uint8_t g_icmp_tx[ICMP_MAX];

void icmp_input(uint32_t src_ip, const uint8_t *payload, uint16_t len) {
    if (payload == NULL || len < 8 || len > ICMP_MAX) {
        return;
    }

    uint8_t type = payload[0];

    /* Log incoming echo replies (responses to our outgoing pings). */
    if (type == ICMP_ECHO_REPLY) {
        uint16_t id  = (uint16_t)((payload[4] << 8) | payload[5]);
        uint16_t seq = (uint16_t)((payload[6] << 8) | payload[7]);
        serial_printf("[icmp] pong from %u.%u.%u.%u id=%u seq=%u\n",
                      (unsigned)((src_ip >> 24) & 0xFF),
                      (unsigned)((src_ip >> 16) & 0xFF),
                      (unsigned)((src_ip >>  8) & 0xFF),
                      (unsigned)( src_ip        & 0xFF),
                      (unsigned)id, (unsigned)seq);
        return;
    }

    if (type != ICMP_ECHO_REQUEST) {
        return;
    }

    /* Build echo reply: copy the request, change type to 0, recompute checksum. */
    for (uint16_t i = 0; i < len; i++) {
        g_icmp_tx[i] = payload[i];
    }
    g_icmp_tx[0] = ICMP_ECHO_REPLY;
    g_icmp_tx[1] = 0;
    g_icmp_tx[2] = 0;
    g_icmp_tx[3] = 0;

    uint16_t csum = inet_checksum(g_icmp_tx, len);
    g_icmp_tx[2] = (uint8_t)(csum >> 8);
    g_icmp_tx[3] = (uint8_t)(csum & 0xFF);

    ipv4_output(src_ip, IPPROTO_ICMP, g_icmp_tx, len);
}

void icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq) {
    uint8_t req[8];

    req[0] = ICMP_ECHO_REQUEST;
    req[1] = 0;
    req[2] = 0;  /* checksum placeholder */
    req[3] = 0;
    req[4] = (uint8_t)(id  >> 8);
    req[5] = (uint8_t)id;
    req[6] = (uint8_t)(seq >> 8);
    req[7] = (uint8_t)seq;

    uint16_t csum = inet_checksum(req, 8);
    req[2] = (uint8_t)(csum >> 8);
    req[3] = (uint8_t)(csum & 0xFF);

    ipv4_output(dst_ip, IPPROTO_ICMP, req, 8);
}
