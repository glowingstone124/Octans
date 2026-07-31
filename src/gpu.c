#include "../include/kernel/gpu.h"

#include "../include/kernel/console_fb.h"
#include "../include/kernel/pci.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/printk.h"
#include "../../include/lampvm/device_abi.h"

#define GPU_TAG "gpu"

static uint32_t g_gpu_control_bar;
static uint32_t g_gpu_caps;
static volatile uint32_t g_gpu_active;
static volatile uint32_t g_gpu_completed_seq;
static volatile uint32_t g_gpu_errors;

static inline uint32_t gpu_read32(uint32_t reg) {
    return *(volatile uint32_t *)(uintptr_t)(g_gpu_control_bar + reg);
}

static inline void gpu_write32(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)(g_gpu_control_bar + reg) = value;
}

void gpu_init(void) {
    const uint32_t control_bar = pci_gpu_bar0();
    const uint32_t vram_bar = pci_gpu_bar1();
    uint32_t caps;

    g_gpu_control_bar = 0u;
    g_gpu_caps = 0u;
    g_gpu_active = 0u;
    g_gpu_completed_seq = 0u;
    g_gpu_errors = 0u;
    if (control_bar == 0u || vram_bar == 0u) {
        KLOGI(GPU_TAG, "PCI display absent, firmware framebuffer retained");
        return;
    }

    g_gpu_control_bar = control_bar;
    caps = gpu_read32(LAMP_GPU_REG_CAPS);
    if (gpu_read32(LAMP_GPU_REG_MAGIC) != LAMP_GPU_MAGIC ||
        gpu_read32(LAMP_GPU_REG_VERSION) != LAMP_GPU_VERSION ||
        (gpu_read32(LAMP_GPU_REG_STATUS) & LAMP_GPU_STATUS_READY) == 0u ||
        gpu_read32(LAMP_GPU_REG_WIDTH) != FB_WIDTH ||
        gpu_read32(LAMP_GPU_REG_HEIGHT) != FB_HEIGHT ||
        gpu_read32(LAMP_GPU_REG_STRIDE) != FB_WIDTH * FB_BPP ||
        gpu_read32(LAMP_GPU_REG_FORMAT) != LAMP_GPU_FORMAT_XRGB8888 ||
        gpu_read32(LAMP_GPU_REG_VRAM_SIZE) < FB_SIZE ||
        (caps & (LAMP_GPU_CAP_DAMAGE | LAMP_GPU_CAP_PAGE_FLIP)) !=
            (LAMP_GPU_CAP_DAMAGE | LAMP_GPU_CAP_PAGE_FLIP)) {
        g_gpu_control_bar = 0u;
        KLOGW(GPU_TAG, "incompatible PCI display, firmware framebuffer retained");
        return;
    }

    if (!console_fb_attach_pci(control_bar, vram_bar)) {
        g_gpu_control_bar = 0u;
        KLOGW(GPU_TAG, "takeover failed, firmware framebuffer retained");
        return;
    }

    g_gpu_completed_seq = gpu_read32(LAMP_GPU_REG_COMPLETE_SEQ);
    g_gpu_caps = caps;
    g_gpu_active = 1u;
    if ((caps & LAMP_GPU_CAP_CURSOR) != 0u) {
        KLOGI(GPU_TAG, "PCI XRGB8888 scanout + cursor plane active");
    } else {
        KLOGI(GPU_TAG, "PCI XRGB8888 scanout active; software cursor fallback");
    }
}

void gpu_irq_handler(void) {
    uint32_t status;
    if (g_gpu_control_bar == 0u) {
        return;
    }
    status = gpu_read32(LAMP_GPU_REG_IRQ_STATUS);
    if ((status & LAMP_GPU_IRQ_FLIP_COMPLETE) != 0u) {
        g_gpu_completed_seq = gpu_read32(LAMP_GPU_REG_COMPLETE_SEQ);
    }
    if ((status & LAMP_GPU_IRQ_ERROR) != 0u) {
        g_gpu_errors++;
    }
    if (status != 0u) {
        gpu_write32(LAMP_GPU_REG_IRQ_ACK, status);
    }
    if ((status & LAMP_GPU_IRQ_ERROR) != 0u) {
        console_fb_detach_pci();
        g_gpu_active = 0u;
        g_gpu_control_bar = 0u;
        g_gpu_caps = 0u;
    }
}

uint32_t gpu_active(void) {
    return g_gpu_active ? 1u : 0u;
}

uint32_t gpu_cursor_available(void) {
    return g_gpu_active && (g_gpu_caps & LAMP_GPU_CAP_CURSOR) != 0u ? 1u : 0u;
}

void gpu_cursor_update(uint32_t x, uint32_t y,
                       uint32_t buttons, uint32_t visible) {
    uint32_t ctrl;
    if (!gpu_cursor_available()) {
        return;
    }
    ctrl = ((buttons & 7u) << LAMP_GPU_CURSOR_BUTTONS_SHIFT);
    if (visible) ctrl |= LAMP_GPU_CURSOR_VISIBLE;
    gpu_write32(LAMP_GPU_REG_CURSOR_X, x);
    gpu_write32(LAMP_GPU_REG_CURSOR_Y, y);
    gpu_write32(LAMP_GPU_REG_CURSOR_CTRL, ctrl);
    gpu_write32(LAMP_GPU_REG_COMMAND, LAMP_GPU_CMD_CURSOR_UPDATE);
}
