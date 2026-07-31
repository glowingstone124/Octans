#ifndef LAMP_KERNEL_NET_H
#define LAMP_KERNEL_NET_H
#include "../../include/kernel/platform.h"
#include "../../include/kernel/types.h"

/* ---- helpers ---- */
static inline void net_copy(uint8_t *d, const uint8_t *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}
static inline void net_zero(uint8_t *d, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) d[i] = 0;
}
static inline uint16_t ntoh16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}
static inline uint32_t ntoh32(uint32_t v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v & 0xFF0000u) >> 8) | ((v >> 24) & 0xFFu);
}
#define hton16(v) ntoh16((uint16_t)(v))
#define hton32(v) ntoh32(v)
static inline uint16_t net_get_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
static inline uint32_t net_get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline void net_put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static inline void net_put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

#define IP_FMT "%u.%u.%u.%u"
#define IP_ARG(ip) ((ip)>>24)&0xFFu, ((ip)>>16)&0xFFu, ((ip)>>8)&0xFFu, (ip)&0xFFu
#define IMAC_ARG(m) (m)[0],(m)[1],(m)[2],(m)[3],(m)[4],(m)[5]

/* Ethernet frame (without preamble/CRC) */
#define ETH_HDR_LEN   14
#define ETH_MTU       1500
#define ETH_TYPE_IP   0x0800u
#define ETH_TYPE_ARP  0x0806u

/* IP protocols */
#define IPPROTO_ICMP  1
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17

/* ARP */
#define ARP_HTYPE_ETH 1
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

/* Guest network config */
#define GUEST_IP   MAKE_IP4(10,0,2,15)
#define GATEWAY_IP MAKE_IP4(10,0,2,2)
#define SUBNET_MASK MAKE_IP4(255,255,255,0)

#define MAKE_IP4(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))

/* ---- Packet buffer ---- */
#define NET_PKT_MAX  2048

typedef struct {
    uint32_t len;
    uint8_t  data[NET_PKT_MAX];
} net_pkt_t;

/* ---- ARP ---- */
typedef struct __attribute__((packed)) {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t op;
    uint8_t  sha[6]; uint32_t spa;
    uint8_t  tha[6]; uint32_t tpa;
} arp_t;

/* ---- Ethernet header ---- */
typedef struct __attribute__((packed)) {
    uint8_t  dst[6], src[6];
    uint16_t etype;
} eth_hdr_t;

/* ---- IP header ---- */
typedef struct __attribute__((packed)) {
    uint8_t  ihl_ver;   /* ver=4 in high nibble, ihl=5 in low */
    uint8_t  dscp_ecn;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl, proto;
    uint16_t csum;
    uint32_t src_ip, dst_ip;
    /* options not supported */
} ip_hdr_t;

/* ---- UDP header ---- */
typedef struct __attribute__((packed)) {
    uint16_t src_port, dst_port;
    uint16_t len, csum;
} udp_hdr_t;

/* ---- TCP header ---- */
typedef struct __attribute__((packed)) {
    uint16_t src_port, dst_port;
    uint32_t seq, ack;
    uint8_t  off_ns;     /* data offset in high nibble */
    uint8_t  flags;
    uint16_t window;
    uint16_t csum, urgent;
} tcp_hdr_t;

#define TCP_F_FIN 0x01u
#define TCP_F_SYN 0x02u
#define TCP_F_RST 0x04u
#define TCP_F_PSH 0x08u
#define TCP_F_ACK 0x10u

/* TCP state */
enum {
    TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED,
    TCP_FIN_WAIT1, TCP_FIN_WAIT2, TCP_CLOSING, TCP_TIME_WAIT
};

typedef struct {
    uint32_t state;
    uint32_t local_ip, remote_ip;
    uint16_t local_port, remote_port;
    uint32_t snd_una, snd_nxt;   /* send seq */
    uint32_t rcv_nxt;            /* recv seq */
    uint8_t  tx_buf[NET_PKT_MAX]; uint32_t tx_len;
    uint8_t  rx_buf[NET_PKT_MAX]; uint32_t rx_len;
} tcp_sock_t;

typedef struct {
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint8_t  rx_buf[NET_PKT_MAX]; uint32_t rx_len;
    uint32_t rx_from_ip; uint16_t rx_from_port;
} udp_sock_t;

typedef struct {
    uint32_t protocol;
} raw_sock_t;

/* ---- Socket type stored in sched_ofile.sock ---- */
enum { SOCK_TYPE_NONE, SOCK_TYPE_TCP, SOCK_TYPE_UDP, SOCK_TYPE_RAW };

typedef struct {
    uint32_t sock_type;
    union { tcp_sock_t tcp; udp_sock_t udp; raw_sock_t raw; };
} net_sock_t;

/* ---- Public API ---- */
void net_init(void);
void net_poll(void);

int  arp_resolve(uint32_t ip, uint8_t *out_mac);
int  ip_send(uint32_t dst_ip, uint8_t proto, const uint8_t *payload, uint32_t plen);
int  ip_recv(const uint8_t *frame, uint32_t flen, uint8_t *proto_out, uint32_t *src_out, const uint8_t **payload, uint32_t *plen);
int  tcp_connect(tcp_sock_t *s, uint32_t dst_ip, uint16_t dst_port);
int  tcp_send(tcp_sock_t *s, const uint8_t *data, uint32_t len);
int  tcp_recv(tcp_sock_t *s, uint8_t *buf, uint32_t max);
int  tcp_close(tcp_sock_t *s);
net_sock_t *net_alloc_sock(void);
void net_free_sock(net_sock_t *s);

int  net_socket(uint32_t domain, uint32_t type, uint32_t protocol, net_sock_t **out);
int  net_connect(net_sock_t *s, uint32_t ip, uint16_t port);
int  net_send(net_sock_t *s, const uint8_t *data, uint32_t len);
int  net_recv(net_sock_t *s, uint8_t *buf, uint32_t max);
int  net_close(net_sock_t *s);

int  net_udp_bind(net_sock_t *s, uint16_t port);
int  net_udp_connect(net_sock_t *s, uint32_t ip, uint16_t port);
int  net_udp_sendto(net_sock_t *s, uint32_t ip, uint16_t port, const uint8_t *d, uint32_t len);
int  net_udp_recvfrom(net_sock_t *s, uint8_t *buf, uint32_t max, uint32_t *from_ip, uint16_t *from_port);
int  net_raw_sendto(net_sock_t *s, uint32_t ip, const uint8_t *d, uint32_t len);
int  net_raw_recvfrom(net_sock_t *s, uint8_t *buf, uint32_t max, uint32_t *from_ip);

/* helpers */
uint16_t net_cksum(const void *data, uint32_t len, uint32_t start);
uint32_t net_parse_ip(const char *s);

/* ethernet driver */
void ether_init(void);
int  ether_send(const uint8_t *frame, uint32_t len);
int  ether_recv(uint8_t *frame, uint32_t max);
int  ether_rx_ready(void);
void ether_irq_handler(void);
void ether_get_mac(uint8_t mac[6]);

#endif
