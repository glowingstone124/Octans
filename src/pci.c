#include "../include/kernel/pci.h"

#include "../include/kernel/platform.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/vm_info.h"
#include "../../include/lampvm/device_abi.h"

#define PCI_TAG "pci"

#define PCI_CFG_VENDOR_DEVICE 0x00u
#define PCI_CFG_COMMAND_STATUS 0x04u
#define PCI_CFG_CLASS_REVISION 0x08u
#define PCI_CFG_HEADER_TYPE 0x0Cu
#define PCI_CFG_BAR0 0x10u
#define PCI_CFG_CAP_PTR 0x34u

#define PCI_COMMAND_MEM_ENABLE 0x0002u
#define PCI_COMMAND_BUS_MASTER 0x0004u
#define PCI_STATUS_CAP_LIST 0x0010u
#define PCI_HEADER_MULTIFUNC 0x80u
#define PCI_BAR_IO 0x01u
#define PCI_BAR_MEM_TYPE_MASK 0x06u
#define PCI_BAR_MEM_TYPE64 0x04u
#define PCI_CAP_ID_MSI 0x05u

#define PCI_CLASS_NETWORK 0x02u
#define PCI_SUBCLASS_ETHERNET 0x00u
#define PCI_CLASS_DISPLAY 0x03u
#define PCI_SUBCLASS_VGA 0x00u
#define PCI_CLASS_MULTIMEDIA 0x04u
#define PCI_SUBCLASS_AUDIO 0x01u
#define LAMP_PCI_ETHER_DEVICE_ID 0x1000u

static uint32_t g_pci_device_count;
static uint32_t g_pci_ether_bar0;
static uint32_t g_pci_gpu_bar0;
static uint32_t g_pci_gpu_bar1;
static uint32_t g_pci_audio_bar0;

#define PCI_MMIO_PAGE_SIZE 0x1000u
#define PCI_MMIO_PAGE_COUNT \
    ((PCI_MMIO_ALLOC_LIMIT - PCI_MMIO_ALLOC_BASE) / PCI_MMIO_PAGE_SIZE)
#define PCI_MMIO_BITMAP_WORDS ((PCI_MMIO_PAGE_COUNT + 31u) / 32u)

static uint32_t g_pci_mmio_pages[PCI_MMIO_BITMAP_WORDS];

static inline uint32_t pci_cfg_addr(uint32_t dev, uint32_t func, uint32_t offset) {
    return PCIE_ECAM_BASE + dev * 0x8000u + func * 0x1000u + (offset & 0xFFCu);
}

static inline uint32_t pci_cfg_read32(uint32_t dev, uint32_t func, uint32_t offset) {
    return *(volatile uint32_t *)(uintptr_t)pci_cfg_addr(dev, func, offset);
}

static inline void pci_cfg_write32(uint32_t dev, uint32_t func,
                                   uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)pci_cfg_addr(dev, func, offset) = value;
}

static uint32_t pci_mmio_range_free(uint32_t first_page, uint32_t page_count) {
    for (uint32_t page = first_page; page < first_page + page_count; page++) {
        if ((g_pci_mmio_pages[page / 32u] & (1u << (page % 32u))) != 0u) {
            return 0u;
        }
    }
    return 1u;
}

static void pci_mmio_mark_range(uint32_t first_page, uint32_t page_count) {
    for (uint32_t page = first_page; page < first_page + page_count; page++) {
        g_pci_mmio_pages[page / 32u] |= 1u << (page % 32u);
    }
}

static uint32_t pci_allocate_mmio(uint32_t size) {
    uint32_t alignment;
    uint32_t page_count;
    uint32_t base;

    if (size == 0u || (size & (size - 1u)) != 0u ||
        size > PCI_MMIO_ALLOC_LIMIT - PCI_MMIO_ALLOC_BASE) {
        return 0u;
    }
    alignment = size > PCI_MMIO_PAGE_SIZE ? size : PCI_MMIO_PAGE_SIZE;
    page_count = (size + PCI_MMIO_PAGE_SIZE - 1u) / PCI_MMIO_PAGE_SIZE;
    base = (PCI_MMIO_ALLOC_BASE + alignment - 1u) & ~(alignment - 1u);
    while (base < PCI_MMIO_ALLOC_LIMIT &&
           size <= PCI_MMIO_ALLOC_LIMIT - base) {
        const uint32_t first_page =
            (base - PCI_MMIO_ALLOC_BASE) / PCI_MMIO_PAGE_SIZE;
        if (first_page + page_count <= PCI_MMIO_PAGE_COUNT &&
            pci_mmio_range_free(first_page, page_count)) {
            pci_mmio_mark_range(first_page, page_count);
            return base;
        }
        base += alignment;
    }
    return 0u;
}

static uint32_t pci_assign_memory_bars(uint32_t dev, uint32_t func, uint32_t bars_out[6]) {
    uint32_t first_bar = 0u;
    uint32_t command_status = pci_cfg_read32(dev, func, PCI_CFG_COMMAND_STATUS);
    uint32_t command = command_status & 0xFFFFu;

    for (uint32_t bar = 0u; bar < 6u; bar++) bars_out[bar] = 0u;
    /* Disable decode while probing, as required by PCI enumeration. */
    pci_cfg_write32(dev, func, PCI_CFG_COMMAND_STATUS,
                    command & ~(PCI_COMMAND_MEM_ENABLE | PCI_COMMAND_BUS_MASTER));
    for (uint32_t bar = 0u; bar < 6u; bar++) {
        uint32_t offset = PCI_CFG_BAR0 + bar * 4u;
        uint32_t original = pci_cfg_read32(dev, func, offset);
        uint32_t probe;
        uint32_t size;
        uint32_t base;
        uint32_t type_bits;

        pci_cfg_write32(dev, func, offset, 0xFFFFFFFFu);
        probe = pci_cfg_read32(dev, func, offset);
        if (probe == 0u || probe == 0xFFFFFFFFu || (probe & PCI_BAR_IO) != 0u) {
            pci_cfg_write32(dev, func, offset, original);
            continue;
        }
        type_bits = probe & 0x0Fu;
        size = (~(probe & ~0x0Fu)) + 1u;
        base = pci_allocate_mmio(size);
        if (base == 0u) {
            pci_cfg_write32(dev, func, offset, original);
            continue;
        }
        pci_cfg_write32(dev, func, offset, base | type_bits);
        bars_out[bar] = base;
        if (first_bar == 0u) first_bar = base;

        if ((type_bits & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE64) {
            if (bar + 1u < 6u) {
                bar++;
                pci_cfg_write32(dev, func, PCI_CFG_BAR0 + bar * 4u, 0u);
            }
        }
    }

    if (first_bar != 0u) {
        command |= PCI_COMMAND_MEM_ENABLE | PCI_COMMAND_BUS_MASTER;
    }
    pci_cfg_write32(dev, func, PCI_CFG_COMMAND_STATUS, command);
    return first_bar;
}

static int pci_enable_msi(uint32_t dev, uint32_t func, uint32_t vector) {
    uint32_t command_status = pci_cfg_read32(dev, func, PCI_CFG_COMMAND_STATUS);
    if (((command_status >> 16) & PCI_STATUS_CAP_LIST) == 0u) return 0;

    uint32_t cap = pci_cfg_read32(dev, func, PCI_CFG_CAP_PTR) & 0xFCu;
    for (uint32_t hops = 0u; cap >= 0x40u && cap < 0x100u && hops < 48u; hops++) {
        uint32_t header = pci_cfg_read32(dev, func, cap);
        uint32_t cap_id = header & 0xFFu;
        uint32_t next = (header >> 8) & 0xFCu;
        if (cap_id == PCI_CAP_ID_MSI) {
            uint32_t control = (header >> 16) & 0xFFFFu;
            pci_cfg_write32(dev, func, cap + 0x4u, 0u); /* destination core 0 */
            if ((control & 0x0080u) != 0u) {
                pci_cfg_write32(dev, func, cap + 0x8u, 0u);
                pci_cfg_write32(dev, func, cap + 0xCu, vector & 0xFFu);
            } else {
                pci_cfg_write32(dev, func, cap + 0x8u, vector & 0xFFu);
            }
            header |= 1u << 16;
            pci_cfg_write32(dev, func, cap, header);
            return 1;
        }
        if (next == 0u || next == cap) break;
        cap = next;
    }
    return 0;
}

static void pci_enumerate_function(uint32_t dev, uint32_t func) {
    uint32_t id = pci_cfg_read32(dev, func, PCI_CFG_VENDOR_DEVICE);
    uint32_t class_revision;
    uint32_t class_code;
    uint32_t subclass;
    uint32_t bar0;
    uint32_t bars[6];

    if ((id & 0xFFFFu) == 0xFFFFu) return;
    g_pci_device_count++;
    class_revision = pci_cfg_read32(dev, func, PCI_CFG_CLASS_REVISION);
    class_code = (class_revision >> 24) & 0xFFu;
    subclass = (class_revision >> 16) & 0xFFu;
    bar0 = pci_assign_memory_bars(dev, func, bars);

    if ((id & 0xFFFFu) == LAMP_PCI_VENDOR_ID &&
        ((id >> 16) & 0xFFFFu) == LAMP_PCI_ETHER_DEVICE_ID &&
        class_code == PCI_CLASS_NETWORK && subclass == PCI_SUBCLASS_ETHERNET) {
        g_pci_ether_bar0 = bar0;
        (void)pci_enable_msi(dev, func, IRQ_ETHER);
    } else if ((id & 0xFFFFu) == LAMP_PCI_VENDOR_ID &&
               ((id >> 16) & 0xFFFFu) == LAMP_PCI_GPU_DEVICE_ID &&
               class_code == PCI_CLASS_DISPLAY && subclass == PCI_SUBCLASS_VGA) {
        g_pci_gpu_bar0 = bars[0];
        g_pci_gpu_bar1 = bars[1];
        (void)pci_enable_msi(dev, func, IRQ_GPU);
    } else if ((id & 0xFFFFu) == LAMP_PCI_VENDOR_ID &&
               ((id >> 16) & 0xFFFFu) == LAMP_PCI_AUDIO_DEVICE_ID &&
               class_code == PCI_CLASS_MULTIMEDIA && subclass == PCI_SUBCLASS_AUDIO) {
        g_pci_audio_bar0 = bars[0];
        (void)pci_enable_msi(dev, func, IRQ_AUDIO);
    }
}

void pci_init(void) {
    boot_info_t info;
    g_pci_device_count = 0u;
    g_pci_ether_bar0 = 0u;
    g_pci_gpu_bar0 = 0u;
    g_pci_gpu_bar1 = 0u;
    g_pci_audio_bar0 = 0u;
    for (uint32_t i = 0u; i < PCI_MMIO_BITMAP_WORDS; i++) {
        g_pci_mmio_pages[i] = 0u;
    }

    if (!vm_info_load_boot(&info) || (info.features & BOOTINFO_FEATURE_PCIE) == 0u) {
        return;
    }
    for (uint32_t dev = 0u; dev < PCI_ECAM_DEV_COUNT; dev++) {
        uint32_t id0 = pci_cfg_read32(dev, 0u, PCI_CFG_VENDOR_DEVICE);
        if ((id0 & 0xFFFFu) == 0xFFFFu) continue;
        uint32_t header = pci_cfg_read32(dev, 0u, PCI_CFG_HEADER_TYPE);
        uint32_t functions = ((header >> 16) & PCI_HEADER_MULTIFUNC) ? 8u : 1u;
        for (uint32_t func = 0u; func < functions; func++) {
            pci_enumerate_function(dev, func);
        }
    }

    if (klog_should_emit(KLOG_LEVEL_INFO)) {
        klog_begin(KLOG_LEVEL_INFO, PCI_TAG);
        klog_puts("devices=");
        klog_hex32(g_pci_device_count);
        klog_puts(" ether_bar0=");
        klog_hex32(g_pci_ether_bar0);
        klog_puts(" gpu=");
        klog_hex32(g_pci_gpu_bar0);
        klog_puts(":");
        klog_hex32(g_pci_gpu_bar1);
        klog_puts(" audio=");
        klog_hex32(g_pci_audio_bar0);
        klog_end();
    }
}

uint32_t pci_ether_bar0(void) {
    return g_pci_ether_bar0;
}

uint32_t pci_device_count(void) {
    return g_pci_device_count;
}

uint32_t pci_gpu_bar0(void) {
    return g_pci_gpu_bar0;
}

uint32_t pci_gpu_bar1(void) {
    return g_pci_gpu_bar1;
}

uint32_t pci_audio_bar0(void) {
    return g_pci_audio_bar0;
}
