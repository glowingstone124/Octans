/* Minimal TCP: synchronous connect/send/recv/close */
#include "net.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/printk.h"

#ifndef LAMP_TCP_TRACE
#define LAMP_TCP_TRACE 0
#endif

typedef struct {
    const uint8_t *payload;
    uint32_t payload_len;
    uint32_t seq;
    uint32_t ack;
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t flags;
} tcp_rx_t;

static int tcp_parse_rx(const uint8_t *frame, uint32_t flen, const tcp_sock_t *s, tcp_rx_t *out) {
    uint8_t proto;
    uint32_t src_ip;
    const uint8_t *seg;
    uint32_t seg_len;
    uint32_t doff;

    if (ip_recv(frame, flen, &proto, &src_ip, &seg, &seg_len) < 0) return -1;
    if (proto != IPPROTO_TCP || seg_len < 20u) return -1;
    doff = (uint32_t)(seg[12] >> 4) * 4u;
    if (doff < 20u || doff > seg_len) return -1;

    out->src_ip = src_ip;
    out->src_port = net_get_be16(seg + 0);
    out->dst_port = net_get_be16(seg + 2);
    if (out->dst_port != s->local_port || out->src_port != s->remote_port) return -1;
    if (s->remote_ip != 0u && src_ip != s->remote_ip) return -1;

    out->seq = net_get_be32(seg + 4);
    out->ack = net_get_be32(seg + 8);
    out->flags = seg[13];
    out->payload = seg + doff;
    out->payload_len = seg_len - doff;
    return 0;
}

static int tcp_build_pkt(uint8_t *frame, uint32_t *flen,
                         uint32_t src_ip, uint32_t dst_ip,
                         uint16_t src_port, uint16_t dst_port,
                         uint32_t seq, uint32_t ack, uint8_t flags,
                         const uint8_t *data, uint32_t dlen) {
    uint8_t my_mac[6], gw_mac[6]; ether_get_mac(my_mac);
    if (arp_resolve(GATEWAY_IP, gw_mac) < 0) return -1;

    /* Ethernet */
    net_copy(frame, gw_mac, 6); net_copy(frame + 6, my_mac, 6);
    frame[12] = 0x08; frame[13] = 0x00;

    /* IP header */
    uint8_t *iph = frame + 14;
    uint32_t tcplen = 20 + dlen;
    uint32_t iplen = 20 + tcplen;
    net_zero(iph, 20);
    iph[0] = 0x45;
    net_put_be16(iph + 2, (uint16_t)iplen);
    iph[8] = 64; iph[9] = IPPROTO_TCP;
    net_put_be32(iph + 12, src_ip);
    net_put_be32(iph + 16, dst_ip);
    uint16_t ipcsum = net_cksum(iph, 20, 0);
    iph[10] = (uint8_t)(ipcsum); iph[11] = (uint8_t)(ipcsum >> 8);

    /* TCP header */
    uint8_t *tcph = iph + 20;
    net_zero(tcph, 20);
    net_put_be16(tcph + 0, src_port);
    net_put_be16(tcph + 2, dst_port);
    net_put_be32(tcph + 4, seq);
    net_put_be32(tcph + 8, ack);
    tcph[12] = 0x50; /* data offset = 5 (20 bytes) */
    tcph[13] = flags;

    net_put_be16(tcph + 14, 4096);

    /* payload */
    if (dlen > 0) net_copy(tcph + 20, data, dlen);

    /* TCP checksum: pseudo-header + TCP segment */
    uint8_t pseudo[12];
    net_put_be32(pseudo + 0, src_ip);
    net_put_be32(pseudo + 4, dst_ip);
    pseudo[8] = 0; pseudo[9] = IPPROTO_TCP;
    net_put_be16(pseudo + 10, (uint16_t)tcplen);
    uint32_t sum = net_cksum(pseudo, 12, 0);
    uint16_t tcpcsum = net_cksum(tcph, tcplen, sum);
    tcph[16] = (uint8_t)(tcpcsum); tcph[17] = (uint8_t)(tcpcsum >> 8);

    *flen = 14 + iplen;
    return 0;
}

int tcp_connect(tcp_sock_t *s, uint32_t dst_ip, uint16_t dst_port) {
    s->local_ip  = GUEST_IP;
    s->remote_ip  = dst_ip;
    s->remote_port = dst_port;
    s->snd_nxt = 0x10000000u;
    s->local_port = 49152 + (((uint16_t)s->snd_nxt) & 0x3FFFu);
    s->rcv_nxt = 0;
    s->state = TCP_CLOSED;

    /* Send SYN */
    uint8_t frame[NET_PKT_MAX]; uint32_t flen;
    s->snd_una = s->snd_nxt;
    if (tcp_build_pkt(frame, &flen, s->local_ip, s->remote_ip,
                      s->local_port, s->remote_port,
                      s->snd_nxt, 0, TCP_F_SYN, 0, 0) < 0) return -1;
    ether_send(frame, flen);
    s->snd_nxt++;
    s->state = TCP_SYN_SENT;

    /* Wait for SYN-ACK — busy-poll so IRQ can deliver between checks */
    for (int retry = 0; retry < 400; retry++) {
        for (volatile int d = 0; d < 5000; d++) {}
        uint8_t rx[NET_PKT_MAX]; int n = ether_recv(rx, sizeof(rx));
        if (n < 54) continue;
        tcp_rx_t in;
        if (tcp_parse_rx(rx, (uint32_t)n, s, &in) < 0) continue;
        if (!(in.flags & TCP_F_SYN) || !(in.flags & TCP_F_ACK)) continue;
        if (in.ack != s->snd_nxt) continue;
        uint32_t rseq = in.seq;
        s->rcv_nxt = rseq + 1;
        s->snd_una = in.ack;

        /* Send ACK */
        if (tcp_build_pkt(frame, &flen, s->local_ip, s->remote_ip,
                          s->local_port, s->remote_port,
                          s->snd_nxt, s->rcv_nxt, TCP_F_ACK, 0, 0) < 0) return -1;
        ether_send(frame, flen);
        s->state = TCP_ESTABLISHED;
        return 0;
    }
    s->state = TCP_CLOSED;
    return -1;
}

int tcp_send(tcp_sock_t *s, const uint8_t *data, uint32_t len) {
    if (s->state != TCP_ESTABLISHED) return -1;
    uint8_t frame[NET_PKT_MAX]; uint32_t flen;
    if (tcp_build_pkt(frame, &flen, s->local_ip, s->remote_ip,
                      s->local_port, s->remote_port,
                      s->snd_nxt, s->rcv_nxt, TCP_F_PSH | TCP_F_ACK,
                      data, len) < 0) return -1;
    ether_send(frame, flen);
    s->snd_nxt += len;
    return (int)len;
}

int tcp_recv(tcp_sock_t *s, uint8_t *buf, uint32_t max) {
    if (s->state != TCP_ESTABLISHED) return -1;

    /* Poll for more data */
    for (int retry = 0; retry < 400; retry++) {
        if (s->rx_len > 0) break;
        for (volatile int __d = 0; __d < 5000; __d++) {}
        uint8_t rx[NET_PKT_MAX]; int n = ether_recv(rx, sizeof(rx));
        if (n < 54) continue;
        tcp_rx_t in;
        if (tcp_parse_rx(rx, (uint32_t)n, s, &in) < 0) continue;
        uint32_t payload_len = in.payload_len;
#if LAMP_TCP_TRACE
        klog_begin(KLOG_LEVEL_INFO, "tcp_rx");
        klog_puts("flags="); klog_hex32(in.flags);
        klog_puts(" plen="); klog_hex32(payload_len);
        klog_puts(" seq="); klog_hex32(in.seq);
        klog_puts(" expect="); klog_hex32(s->rcv_nxt);
        klog_end();
#endif
        if (payload_len > 0) {
            const uint8_t *pld = in.payload;
            uint32_t rseq = in.seq;
            if (rseq == s->rcv_nxt) {
                uint32_t store = payload_len;
                uint32_t avail = (uint32_t)sizeof(s->rx_buf) - s->rx_len;
                if (store > avail) store = avail;
                s->rcv_nxt += payload_len;
                if (store != 0u) {
                    net_copy(s->rx_buf + s->rx_len, pld, store);
                    s->rx_len += store;
                }
                if (in.flags & TCP_F_FIN) {
                    s->rcv_nxt++;
                    s->state = TCP_CLOSED;
                }
                /* Send ACK */
                uint8_t frm[NET_PKT_MAX]; uint32_t fl;
                tcp_build_pkt(frm, &fl, s->local_ip, s->remote_ip,
                              s->local_port, s->remote_port,
                              s->snd_nxt, s->rcv_nxt, TCP_F_ACK, 0, 0);
                ether_send(frm, fl);
                break;
            }
        }
        if (in.flags & TCP_F_FIN) {
            s->rcv_nxt++;
            /* Send FIN-ACK */
            uint8_t frm[NET_PKT_MAX]; uint32_t fl;
            tcp_build_pkt(frm, &fl, s->local_ip, s->remote_ip,
                          s->local_port, s->remote_port,
                          s->snd_nxt, s->rcv_nxt, TCP_F_ACK, 0, 0);
            ether_send(frm, fl);
            s->state = TCP_CLOSED;
            break;
        }
    }

    if (s->rx_len == 0) return 0;
    uint32_t n = s->rx_len; if (n > max) n = max;
    net_copy(buf, s->rx_buf, n);
    /* shift remaining */
    if (n < s->rx_len) {
        for (uint32_t i = 0; i < s->rx_len - n; i++) s->rx_buf[i] = s->rx_buf[n + i];
    }
    s->rx_len -= n;
    return (int)n;
}

int tcp_close(tcp_sock_t *s) {
    if (s->state != TCP_ESTABLISHED) return 0;
    s->state = TCP_FIN_WAIT1;
    uint8_t frame[NET_PKT_MAX]; uint32_t flen;
    if (tcp_build_pkt(frame, &flen, s->local_ip, s->remote_ip,
                      s->local_port, s->remote_port,
                      s->snd_nxt, s->rcv_nxt, TCP_F_FIN | TCP_F_ACK, 0, 0) < 0)
        return -1;
    ether_send(frame, flen);
    s->snd_nxt++;
    s->state = TCP_CLOSED;
    return 0;
}
