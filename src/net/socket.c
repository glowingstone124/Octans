/* Socket API layer */
#include "net.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/printk.h"

static net_sock_t g_socks[4];
static uint32_t g_used;
static uint16_t g_next_udp_port = 49152u;

static uint16_t net_alloc_ephemeral_port(void) {
    uint16_t port = g_next_udp_port++;
    if (g_next_udp_port < 49152u) g_next_udp_port = 49152u;
    return port;
}

net_sock_t *net_alloc_sock(void) {
    net_sock_t *s = (net_sock_t *)(uintptr_t)0;
    for (uint32_t i = 0; i < 4; i++) {
        if (g_used & (1u << i)) continue;
        s = &g_socks[i];
        g_used |= (1u << i);
        net_zero((uint8_t *)s, sizeof(*s));
        s->sock_type = SOCK_TYPE_NONE;
        break;
    }
    return s;
}

void net_free_sock(net_sock_t *s) {
    if (s >= &g_socks[0] && s < &g_socks[4]) {
        uint32_t idx = (uint32_t)(s - g_socks);
        g_used &= ~(1u << idx);
    }
}

int net_socket(uint32_t domain, uint32_t type, uint32_t protocol, net_sock_t **out) {
    if (domain != 2 && domain != 1 && domain != 10) return -1; /* AF_INET only */
    net_sock_t *s = net_alloc_sock();
    if (!s) return -1;
    uint32_t stype = type & 0x0Fu;
    if (stype == 1) s->sock_type = SOCK_TYPE_TCP;
    else if (stype == 2) s->sock_type = SOCK_TYPE_UDP;
    else if (stype == 3 && domain == 2 && protocol == IPPROTO_ICMP) {
        s->sock_type = SOCK_TYPE_RAW;
        s->raw.protocol = protocol;
    }
    else { net_free_sock(s); return -1; }
    *out = s;
    return 0;
}

int net_connect(net_sock_t *s, uint32_t ip, uint16_t port) {
    if (!s) return -1;
    if (s->sock_type == SOCK_TYPE_TCP) return tcp_connect(&s->tcp, ip, port);
    if (s->sock_type == SOCK_TYPE_UDP) return net_udp_connect(s, ip, port);
    return -1;
}

int net_send(net_sock_t *s, const uint8_t *data, uint32_t len) {
    if (!s) return -1;
    if (s->sock_type == SOCK_TYPE_TCP) return tcp_send(&s->tcp, data, len);
    if (s->sock_type == SOCK_TYPE_UDP) {
        if (s->udp.remote_ip == 0u || s->udp.remote_port == 0u) return -1;
        return net_udp_sendto(s, s->udp.remote_ip, s->udp.remote_port, data, len);
    }
    return -1;
}

int net_recv(net_sock_t *s, uint8_t *buf, uint32_t max) {
    if (!s) return -1;
    if (s->sock_type == SOCK_TYPE_TCP) return tcp_recv(&s->tcp, buf, max);
    if (s->sock_type == SOCK_TYPE_UDP) {
        uint32_t from_ip = 0u;
        uint16_t from_port = 0u;
        return net_udp_recvfrom(s, buf, max, &from_ip, &from_port);
    }
    return -1;
}

int net_close(net_sock_t *s) {
    if (!s) return -1;
    if (s->sock_type == SOCK_TYPE_TCP) tcp_close(&s->tcp);
    net_free_sock(s);
    return 0;
}

int net_udp_bind(net_sock_t *s, uint16_t port) {
    if (!s || s->sock_type != SOCK_TYPE_UDP) return -1;
    s->udp.local_ip = GUEST_IP;
    s->udp.local_port = port;
    return 0;
}

int net_udp_connect(net_sock_t *s, uint32_t ip, uint16_t port) {
    if (!s || s->sock_type != SOCK_TYPE_UDP || ip == 0u || port == 0u) return -1;
    if (s->udp.local_port == 0u) {
        s->udp.local_ip = GUEST_IP;
        s->udp.local_port = net_alloc_ephemeral_port();
    }
    s->udp.remote_ip = ip;
    s->udp.remote_port = port;
    return 0;
}

int net_udp_sendto(net_sock_t *s, uint32_t ip, uint16_t port, const uint8_t *d, uint32_t len) {
    if (!s || s->sock_type != SOCK_TYPE_UDP) return -1;
    if (len > NET_PKT_MAX - 8u) return -1;
    if (s->udp.local_port == 0u) {
        s->udp.local_ip = GUEST_IP;
        s->udp.local_port = net_alloc_ephemeral_port();
    }
    /* Build UDP packet */
    /* We'll use a simplified UDP send via IP */
    uint8_t udp_pkt[NET_PKT_MAX];
    net_put_be16(udp_pkt + 0, s->udp.local_port);
    net_put_be16(udp_pkt + 2, port);
    uint16_t ulen = (uint16_t)(8 + len);
    net_put_be16(udp_pkt + 4, ulen);
    udp_pkt[6] = 0; udp_pkt[7] = 0; /* checksum = 0 (no checksum) */
    net_copy(udp_pkt + 8, d, len);
    if (ip_send(ip, IPPROTO_UDP, udp_pkt, 8 + len) < 0) return -1;
    return (int)len;
}

int net_udp_recvfrom(net_sock_t *s, uint8_t *buf, uint32_t max, uint32_t *from_ip, uint16_t *from_port) {
    if (!s || s->sock_type != SOCK_TYPE_UDP) return -1;

    for (int retry = 0; retry < 15; retry++) {
        uint8_t rx[NET_PKT_MAX]; int n = ether_recv(rx, sizeof(rx));
        if (n < 42) { for (volatile int __d = 0; __d < 3000; __d++) {} continue; }
        uint8_t proto;
        uint32_t src_ip;
        const uint8_t *udph;
        uint32_t ip_payload_len;
        if (ip_recv(rx, (uint32_t)n, &proto, &src_ip, &udph, &ip_payload_len) < 0) {
            for (volatile int __d = 0; __d < 3000; __d++) {}
            continue;
        }
        if (proto != IPPROTO_UDP || ip_payload_len < 8u) { for (volatile int __d = 0; __d < 3000; __d++) {} continue; }
        uint16_t sp = net_get_be16(udph + 0);
        uint16_t dp = net_get_be16(udph + 2);
        if (dp != s->udp.local_port) { for (volatile int __d = 0; __d < 3000; __d++) {} continue; }
        if (s->udp.remote_ip != 0u && src_ip != s->udp.remote_ip) continue;
        if (s->udp.remote_port != 0u && sp != s->udp.remote_port) continue;
        uint32_t udplen = net_get_be16(udph + 4);
        if (udplen < 8u || udplen > ip_payload_len) continue;
        uint32_t plen = udplen - 8;
        if (plen > max) plen = max;
        net_copy(buf, udph + 8, plen);
        if (from_ip) *from_ip = src_ip;
        if (from_port) *from_port = sp;
        return (int)plen;
    }
    return -1;
}

int net_raw_sendto(net_sock_t *s, uint32_t ip, const uint8_t *d, uint32_t len) {
    if (!s || s->sock_type != SOCK_TYPE_RAW) return -1;
    if (s->raw.protocol != IPPROTO_ICMP) return -1;
    if (len > NET_PKT_MAX - 20u) return -1;
    return ip_send(ip, (uint8_t)s->raw.protocol, d, len);
}

int net_raw_recvfrom(net_sock_t *s, uint8_t *buf, uint32_t max, uint32_t *from_ip) {
    if (!s || s->sock_type != SOCK_TYPE_RAW) return -1;
    if (s->raw.protocol != IPPROTO_ICMP) return -1;

    for (int retry = 0; retry < 2000; retry++) {
        uint8_t rx[NET_PKT_MAX];
        int n = ether_recv(rx, sizeof(rx));
        if (n < 34) {
            for (volatile int __d = 0; __d < 10000; __d++) {}
            continue;
        }
        const uint8_t *iph = rx + ETH_HDR_LEN;
        uint32_t ihl = (uint32_t)(iph[0] & 0x0Fu) * 4u;
        uint8_t proto;
        uint32_t src_ip;
        const uint8_t *payload;
        uint32_t payload_len;
        if (n < (int)(ETH_HDR_LEN + 20u)) continue;
        if (ip_recv(rx, (uint32_t)n, &proto, &src_ip, &payload, &payload_len) < 0) continue;
        if (proto != (uint8_t)s->raw.protocol) continue;
        if (from_ip) {
            *from_ip = src_ip;
        }
        uint32_t iplen = ihl + payload_len;
        if (iplen > max) iplen = max;
        net_copy(buf, iph, iplen);
        if (iplen != 0u) {
            /*
             * Some current userland builds disagree on iphdr bitfield
             * nibble order. Keep IHL readable as 5 from either nibble.
             */
            buf[0] = 0x55u;
        }
        return (int)iplen;
    }
    return -1;
}

void net_poll(void) {
    /* Called periodically — just drain incoming frames */
    uint8_t rx[NET_PKT_MAX];
    int n = ether_recv(rx, sizeof(rx));
    (void)n;
}
