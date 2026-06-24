#include "../include/kernel/iommu.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/vm_info.h"

#define IOMMU_TAG "iommu"
#define IOMMU_DMA_IOVA_BASE 0x01000000u
#define IOMMU_PAGE_SIZE 4096u
#define IOMMU_L1_ENTRIES 1024u
#define IOMMU_L2_ENTRIES 1024u
#define IOMMU_PDE_SHIFT 22u
#define IOMMU_PTE_SHIFT 12u
#define IOMMU_DMA_PAGES (KERNEL_MEM_SIZE / IOMMU_PAGE_SIZE)
#define IOMMU_DMA_L2_TABLES ((IOMMU_DMA_PAGES + IOMMU_L2_ENTRIES - 1u) / IOMMU_L2_ENTRIES)

static volatile uint32_t g_iommu_active;
static volatile uint32_t g_iommu_initialized;
static uint32_t g_iommu_l1[IOMMU_L1_ENTRIES] __attribute__((aligned(4096)));
static uint32_t g_iommu_l2[IOMMU_DMA_L2_TABLES][IOMMU_L2_ENTRIES] __attribute__((aligned(4096)));

static inline void mmio_write32(uint32_t addr, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)addr = value;
}

static inline uint32_t mmio_read32(uint32_t addr) {
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static void iommu_build_disk_page_table(void) {
    for (uint32_t i = 0u; i < IOMMU_L1_ENTRIES; i++) {
        g_iommu_l1[i] = 0u;
    }
    for (uint32_t i = 0u; i < IOMMU_DMA_L2_TABLES; i++) {
        for (uint32_t j = 0u; j < IOMMU_L2_ENTRIES; j++) {
            g_iommu_l2[i][j] = 0u;
        }
    }

    for (uint32_t page = 0u; page < IOMMU_DMA_PAGES; page++) {
        uint32_t iova = IOMMU_DMA_IOVA_BASE + page * IOMMU_PAGE_SIZE;
        uint32_t pa = page * IOMMU_PAGE_SIZE;
        uint32_t pde = iova >> IOMMU_PDE_SHIFT;
        uint32_t pte = (iova >> IOMMU_PTE_SHIFT) & 0x3FFu;
        uint32_t table = page / IOMMU_L2_ENTRIES;

        g_iommu_l2[table][pte] = (pa & 0xFFFFF000u) | IOMMU_PTE_P;
        g_iommu_l1[pde] = ((uint32_t)(uintptr_t)&g_iommu_l2[table][0] & 0xFFFFF000u) | IOMMU_PTE_P;
    }
}

static void iommu_program_device(uint32_t dev_id) {
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_DEVSEL, dev_id);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_DEV_CTRL, 0u);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_IOVA_BASE_LO, IOMMU_DMA_IOVA_BASE);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_IOVA_BASE_HI, 0u);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_IOVA_SIZE, KERNEL_MEM_SIZE);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_PA_BASE_LO, 0u);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_PA_BASE_HI, 0u);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_ROOT_LO, (uint32_t)(uintptr_t)&g_iommu_l1[0]);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_ROOT_HI, 0u);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_DEV_CTRL, IOMMU_DEV_CTRL_ENABLE | IOMMU_DEV_CTRL_PAGED);
}

void iommu_init(void) {
    boot_info_t info;
    uint32_t cap;
    uint32_t devs;

    if (g_iommu_initialized) {
        return;
    }
    g_iommu_initialized = 1u;
    g_iommu_active = 0u;
    if (!vm_info_load_boot(&info)) {
        KLOGW(IOMMU_TAG, "bootinfo missing, disabled");
        return;
    }
    if ((info.features & BOOTINFO_FEATURE_IOMMU_MMIO) == 0u) {
        KLOGI(IOMMU_TAG, "feature absent, bypass");
        return;
    }

    cap = mmio_read32(IOMMU_MMIO_BASE + IOMMU_REG_CAP);
    devs = cap & 0xFFu;
    if (devs <= IOMMU_DEV_ETHER) {
        KLOGW(IOMMU_TAG, "invalid cap, bypass");
        return;
    }

    iommu_build_disk_page_table();

    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_CTRL, 0u);
    iommu_program_device(IOMMU_DEV_DISK);
    iommu_program_device(IOMMU_DEV_ETHER);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_FAULT_STATUS, 1u);
    mmio_write32(IOMMU_MMIO_BASE + IOMMU_REG_CTRL, IOMMU_CTRL_ENABLE);

    g_iommu_active = 1u;
    KLOGI(IOMMU_TAG, "enabled paged iova for disk/ether");
}

uint32_t iommu_active(void) {
    return g_iommu_active ? 1u : 0u;
}

uint32_t iommu_dma_iova(uint32_t pa_addr, uint32_t len, uint32_t *iova_out) {
    uint32_t iova;
    if (!iova_out || len == 0u) {
        return 0u;
    }
    if (pa_addr >= KERNEL_MEM_SIZE || len > (KERNEL_MEM_SIZE - pa_addr)) {
        return 0u;
    }
    if (!g_iommu_active) {
        *iova_out = pa_addr;
        return 1u;
    }
    if (pa_addr > (0xFFFFFFFFu - IOMMU_DMA_IOVA_BASE)) {
        return 0u;
    }
    iova = IOMMU_DMA_IOVA_BASE + pa_addr;
    *iova_out = iova;
    return 1u;
}
