/* IPv4 layer */
#include "net.h"
#include "../../include/kernel/sched.h"

static uint16_t g_ip_id;

uint16_t net_cksum(const void *data, uint32_t len, uint32_t start) {
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = start;
    for (uint32_t i = 0; i < len / 2; i++) sum += p[i];
    if (len & 1) sum += ((const uint8_t *)data)[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

int ip_send(uint32_t dst_ip, uint8_t proto, const uint8_t *payload, uint32_t plen) {
    uint8_t gw_mac[6];
    if (arp_resolve(GATEWAY_IP, gw_mac) < 0) return -1;

    uint8_t my_mac[6]; ether_get_mac(my_mac);

    uint8_t frame[NET_PKT_MAX];
    /* Ethernet header */
    net_copy(frame, gw_mac, 6);
    net_copy(frame + 6, my_mac, 6);
    frame[12] = 0x08; frame[13] = 0x00; /* IP */

    /* IP header at offset 14 */
    uint32_t iplen = 20 + plen;
    uint8_t *iph = frame + 14;
    net_zero(iph, 24);
    iph[0] = 0x45; /* IPv4, IHL=5 */
    net_put_be16(iph + 2, (uint16_t)iplen);
    uint16_t id = g_ip_id++;
    net_put_be16(iph + 4, id);
    iph[8] = 64; /* TTL */
    iph[9] = proto;
    uint32_t sip = GUEST_IP;
    net_put_be32(iph + 12, sip);
    net_put_be32(iph + 16, dst_ip);
    uint16_t csum = net_cksum(iph, 20, 0);
    iph[10] = (uint8_t)(csum); iph[11] = (uint8_t)(csum >> 8);

    net_copy(frame + 34, payload, plen);
    return ether_send(frame, 14 + iplen);
}

int ip_recv(const uint8_t *frame, uint32_t flen, uint8_t *proto_out, uint32_t *src_out,
            const uint8_t **payload, uint32_t *plen) {
    if (flen < 34) return -1;
    if (frame[12] != 0x08 || frame[13] != 0x00) return -1;
    const uint8_t *iph = frame + 14;
    if ((iph[0] & 0xF0) != 0x40) return -1; /* IPv4 */
    uint32_t ihl = (uint32_t)(iph[0] & 0x0Fu) * 4u;
    uint32_t iplen = net_get_be16(iph + 2);
    if (ihl < 20u || iplen < ihl || 14u + iplen > flen) return -1;
    if (net_cksum(iph, ihl, 0) != 0u) return -1;
    if ((net_get_be16(iph + 6) & 0x3FFFu) != 0u) return -1; /* no fragments */
    /* Check destination */
    uint32_t dst = net_get_be32(iph + 16);
    if (dst != GUEST_IP) return -1;
    *proto_out = iph[9];
    *src_out   = net_get_be32(iph + 12);
    *payload   = iph + ihl;
    *plen      = iplen - ihl;
    return 0;
}
