/* ARP: resolve gateway MAC. Polls with retry. */
#include "net.h"
#include "../../include/kernel/sched.h"
#include "../../include/kernel/printk.h"
#include "../../include/kernel/sched.h"

static uint8_t g_gw_mac[6];
static uint32_t g_gw_mac_valid;

int arp_resolve(uint32_t ip, uint8_t *out_mac) {
    if (g_gw_mac_valid && ip == GATEWAY_IP) {
        net_copy(out_mac, g_gw_mac, 6);
        return 0;
    }
    /* Build ARP request */
    uint8_t my_mac[6]; ether_get_mac(my_mac);
    uint8_t req[64]; uint32_t rlen = 0;

    /* eth header */
    net_zero(req, 6); net_copy(req + 6, my_mac, 6); /* dst=ff:ff:ff:ff:ff:ff */
    for (int i = 0; i < 6; i++) req[i] = 0xFF;
    net_copy(req + 6, my_mac, 6);
    req[12] = 0x08; req[13] = 0x06; /* ARP */

    /* ARP body at offset 14 */
    uint8_t *a = req + 14;
    a[0] = 0x00; a[1] = 0x01; /* htype=ETH */
    a[2] = 0x08; a[3] = 0x00; /* ptype=IP */
    a[4] = 6; a[5] = 4;        /* hlen=6, plen=4 */
    a[6] = 0x00; a[7] = 0x01; /* op=REQUEST */
    net_copy(a + 8,  my_mac, 6);             /* sha */
    uint32_t sip = GUEST_IP;
    a[14] = (uint8_t)(sip >> 24); a[15] = (uint8_t)(sip >> 16);
    a[16] = (uint8_t)(sip >> 8);  a[17] = (uint8_t)(sip);
    net_zero(a + 18, 6);                      /* tha = 0 */
    uint32_t tip = ip;
    a[24] = (uint8_t)(tip >> 24); a[25] = (uint8_t)(tip >> 16);
    a[26] = (uint8_t)(tip >> 8);  a[27] = (uint8_t)(tip);
    ether_send(req, 42);

    for (int retry = 0; retry < 30; retry++) {
        for (volatile int __d = 0; __d < 5000; __d++) {}
        uint8_t buf[128]; int n = ether_recv(buf, sizeof(buf));
        if (n < 42) continue;
        if (buf[12] != 0x08 || buf[13] != 0x06) continue;
        uint8_t *arph = buf + 14;
        if (arph[6] != 0x00 || arph[7] != 0x02) continue; /* not REPLY */
        uint32_t rspa = ((uint32_t)arph[14] << 24) | ((uint32_t)arph[15] << 16) |
                        ((uint32_t)arph[16] << 8)  | (uint32_t)arph[17];
        if (rspa != ip) continue;
        net_copy(g_gw_mac, arph + 8, 6);
        g_gw_mac_valid = 1;
        net_copy(out_mac, g_gw_mac, 6);
        return 0;
    }
    return -1;
}
