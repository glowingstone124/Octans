/* Ethernet MMIO driver */
#include "net.h"
#include "../../include/kernel/iommu.h"
#include "../../include/kernel/pci.h"
#include "../../include/kernel/printk.h"
#include "../../include/kernel/sched.h"

#define ETHER_BASE 0x00750000u
#define REG_TX_LEN  0x00u
#define REG_TX_LO   0x04u
#define REG_RX_LEN  0x08u
#define REG_RX_LO   0x0Cu
#define REG_STATUS  0x10u
#define REG_MAC_LO  0x14u
#define REG_MAC_HI  0x18u

#define STATUS_LINK      0x01u
#define STATUS_RX_READY  0x02u

static uint8_t  g_ether_mac[6];
static uint32_t g_rx_buf_phys;
static uint32_t g_rx_buf_iova;
static uint8_t  g_rx_ring[2048]; /* one-frame RX ring */
static uint32_t g_rx_len;
static uint32_t g_ether_base = ETHER_BASE;

static inline uint32_t mmio_read32(uint32_t addr) {
    return *(volatile uint32_t *)(uintptr_t)addr;
}
static inline void mmio_write32(uint32_t addr, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)addr = val;
}

void ether_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = g_ether_mac[i];
}

void ether_init(void) {
    uint32_t pci_base = pci_ether_bar0();
    if (pci_base != 0u) g_ether_base = pci_base;
    uint32_t ml = mmio_read32(g_ether_base + REG_MAC_LO);
    uint32_t mh = mmio_read32(g_ether_base + REG_MAC_HI);
    g_ether_mac[0] = (uint8_t)(ml);       g_ether_mac[1] = (uint8_t)(ml >> 8);
    g_ether_mac[2] = (uint8_t)(ml >> 16); g_ether_mac[3] = (uint8_t)(ml >> 24);
    g_ether_mac[4] = (uint8_t)(mh);       g_ether_mac[5] = (uint8_t)(mh >> 8);

    g_rx_buf_phys = (uint32_t)(uintptr_t)&g_rx_ring[0];
    if (!iommu_dma_iova(g_rx_buf_phys, sizeof(g_rx_ring), &g_rx_buf_iova)) {
        g_rx_buf_iova = g_rx_buf_phys;
    }
    g_rx_len = 0;
    mmio_write32(g_ether_base + REG_RX_LO, g_rx_buf_iova);

    klog_begin(KLOG_LEVEL_INFO, "ether");
    klog_puts("mac=");
    for (int i = 0; i < 6; i++) { klog_hex32(g_ether_mac[i]); if (i<5) klog_puts(":"); }
    klog_end();
}

int ether_send(const uint8_t *frame, uint32_t len) {
    uint32_t tx_iova;
    if (len < 14 || len > 1514) return -1;
    if (!iommu_dma_iova((uint32_t)(uintptr_t)frame, len, &tx_iova)) return -1;
    mmio_write32(g_ether_base + REG_TX_LO, tx_iova);
    mmio_write32(g_ether_base + REG_TX_LEN, len);
    return 0;
}

int ether_rx_ready(void) {
    if (g_rx_len != 0u) return 1;
    return (mmio_read32(g_ether_base + REG_STATUS) & STATUS_RX_READY) != 0u;
}

int ether_recv(uint8_t *frame, uint32_t max) {
    /* Poll MMIO directly — IRQs may be masked during syscalls */
    if (g_rx_len == 0u) {
        uint32_t status = mmio_read32(g_ether_base + REG_STATUS);
        if (status & STATUS_RX_READY) {
            uint32_t rx_len = mmio_read32(g_ether_base + REG_RX_LEN);
            if (rx_len > 0 && rx_len <= sizeof(g_rx_ring)) {
                g_rx_len = rx_len;
            }
        }
    }
    if (g_rx_len == 0) return 0;
    uint32_t n = g_rx_len;
    if (n > max) n = max;
    for (uint32_t i = 0; i < n; i++) frame[i] = g_rx_ring[i];
    g_rx_len = 0;
    /* Release the DMA buffer only after its contents have been copied. */
    mmio_write32(g_ether_base + REG_RX_LO, g_rx_buf_iova);
    return (int)n;
}

void ether_irq_handler(void) {
    /* Keep the device frame pending until the software slot is available. */
    if (g_rx_len != 0u) return;
    uint32_t status = mmio_read32(g_ether_base + REG_STATUS);
    if ((status & STATUS_RX_READY) == 0) return;
    uint32_t rx_len = mmio_read32(g_ether_base + REG_RX_LEN);
    if (rx_len > 0 && rx_len <= sizeof(g_rx_ring)) {
        g_rx_len = rx_len;
    }
    /* Leave RX_READY asserted: ether_recv() acknowledges after copying. */
}
