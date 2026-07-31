#include "../include/kernel/audio.h"

#include "../include/kernel/dma_ring.h"
#include "../include/kernel/pci.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/spinlock.h"
#include "../../include/lampvm/device_abi.h"

#define AUDIO_TAG "audio"
#define AUDIO_RING_COUNT 16u

static lamp_dma_desc_t g_audio_submissions[AUDIO_RING_COUNT]
    __attribute__((aligned(64)));
static lamp_dma_completion_t g_audio_completions[AUDIO_RING_COUNT]
    __attribute__((aligned(64)));
static int16_t g_audio_probe_silence[64u * LAMP_AUDIO_CHANNELS]
    __attribute__((aligned(64)));
static dma_ring_t g_audio_ring;
static spinlock_t g_audio_lock;
static uint32_t g_audio_bar;
static volatile uint32_t g_audio_active;
static volatile uint32_t g_audio_completed;
static volatile uint32_t g_audio_errors;

static inline uint32_t audio_read32(uint32_t reg) {
    return *(volatile uint32_t *)(uintptr_t)(g_audio_bar + reg);
}

static inline void audio_write32(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)(g_audio_bar + reg) = value;
}

static void audio_reap_locked(void) {
    lamp_dma_completion_t completion;
    uint32_t device_tail;
    if (!g_audio_active) {
        return;
    }
    device_tail = audio_read32(LAMP_AUDIO_REG_COMPLETE_TAIL);
    while (dma_ring_reap(&g_audio_ring, device_tail, &completion)) {
        if (completion.status == LAMP_DMA_COMPLETION_OK) {
            g_audio_completed++;
        } else {
            g_audio_errors++;
        }
    }
    audio_write32(LAMP_AUDIO_REG_COMPLETE_HEAD, g_audio_ring.completion_head);
}

void audio_init(void) {
    const uint32_t bar = pci_audio_bar0();
    uint32_t caps;

    g_audio_bar = 0u;
    g_audio_active = 0u;
    g_audio_completed = 0u;
    g_audio_errors = 0u;
    spinlock_init(&g_audio_lock);
    if (bar == 0u) {
        KLOGI(AUDIO_TAG, "PCI audio absent");
        return;
    }
    g_audio_bar = bar;
    caps = audio_read32(LAMP_AUDIO_REG_CAPS);
    if (audio_read32(LAMP_AUDIO_REG_MAGIC) != LAMP_AUDIO_MAGIC ||
        audio_read32(LAMP_AUDIO_REG_VERSION) != LAMP_AUDIO_VERSION ||
        (audio_read32(LAMP_AUDIO_REG_STATUS) & LAMP_AUDIO_STATUS_READY) == 0u ||
        audio_read32(LAMP_AUDIO_REG_RATE) != LAMP_AUDIO_RATE ||
        audio_read32(LAMP_AUDIO_REG_CHANNELS) != LAMP_AUDIO_CHANNELS ||
        audio_read32(LAMP_AUDIO_REG_SAMPLE_BITS) != LAMP_AUDIO_SAMPLE_BITS ||
        audio_read32(LAMP_AUDIO_REG_FRAME_BYTES) != LAMP_AUDIO_FRAME_BYTES ||
        (caps & (LAMP_AUDIO_CAP_PLAYBACK | LAMP_AUDIO_CAP_DMA_RING |
                 LAMP_AUDIO_CAP_COMPLETION_RING | LAMP_AUDIO_CAP_IOMMU)) !=
                (LAMP_AUDIO_CAP_PLAYBACK | LAMP_AUDIO_CAP_DMA_RING |
                 LAMP_AUDIO_CAP_COMPLETION_RING | LAMP_AUDIO_CAP_IOMMU) ||
        !dma_ring_init(&g_audio_ring,
                       g_audio_submissions, AUDIO_RING_COUNT,
                       g_audio_completions, AUDIO_RING_COUNT)) {
        g_audio_bar = 0u;
        KLOGW(AUDIO_TAG, "incompatible PCI audio device");
        return;
    }

    audio_write32(LAMP_AUDIO_REG_COMMAND, LAMP_AUDIO_CMD_RESET);
    audio_write32(LAMP_AUDIO_REG_SUBMIT_BASE_LO, g_audio_ring.submission_iova);
    audio_write32(LAMP_AUDIO_REG_SUBMIT_BASE_HI, 0u);
    audio_write32(LAMP_AUDIO_REG_SUBMIT_COUNT, g_audio_ring.submission_count);
    audio_write32(LAMP_AUDIO_REG_COMPLETE_BASE_LO, g_audio_ring.completion_iova);
    audio_write32(LAMP_AUDIO_REG_COMPLETE_BASE_HI, 0u);
    audio_write32(LAMP_AUDIO_REG_COMPLETE_COUNT, g_audio_ring.completion_count);
    audio_write32(LAMP_AUDIO_REG_COMPLETE_HEAD, 0u);
    audio_write32(LAMP_AUDIO_REG_IRQ_ACK,
                  LAMP_AUDIO_IRQ_COMPLETION | LAMP_AUDIO_IRQ_ERROR);
    audio_write32(LAMP_AUDIO_REG_IRQ_ENABLE,
                  LAMP_AUDIO_IRQ_COMPLETION | LAMP_AUDIO_IRQ_ERROR);
    audio_write32(LAMP_AUDIO_REG_COMMAND, LAMP_AUDIO_CMD_ENABLE);
    if ((audio_read32(LAMP_AUDIO_REG_STATUS) & LAMP_AUDIO_STATUS_RUNNING) == 0u) {
        audio_write32(LAMP_AUDIO_REG_COMMAND, LAMP_AUDIO_CMD_DISABLE);
        g_audio_bar = 0u;
        KLOGW(AUDIO_TAG, "DMA ring enable failed");
        return;
    }

    g_audio_active = 1u;
    (void)audio_submit_pcm(g_audio_probe_silence, 64u, 0xA11D0001u);
    KLOGI(AUDIO_TAG, "48 kHz S16 stereo DMA playback ready");
}

uint32_t audio_submit_pcm(const int16_t *samples, uint32_t frames,
                          uint32_t cookie) {
    uint32_t bytes;
    uint32_t device_head;
    if (!g_audio_active || !samples || frames == 0u ||
        frames > LAMP_AUDIO_MAX_BUFFER_BYTES / LAMP_AUDIO_FRAME_BYTES) {
        return 0u;
    }
    spinlock_lock(&g_audio_lock);
    if (!g_audio_active) {
        spinlock_unlock(&g_audio_lock);
        return 0u;
    }
    audio_reap_locked();
    bytes = frames * LAMP_AUDIO_FRAME_BYTES;
    device_head = audio_read32(LAMP_AUDIO_REG_SUBMIT_HEAD);
    if (!dma_ring_submit(&g_audio_ring, device_head,
                         (uint32_t)(uintptr_t)samples, bytes,
                         LAMP_DMA_DESC_F_IRQ | LAMP_DMA_DESC_F_END, cookie)) {
        spinlock_unlock(&g_audio_lock);
        return 0u;
    }
    audio_write32(LAMP_AUDIO_REG_SUBMIT_TAIL, g_audio_ring.submission_tail);
    spinlock_unlock(&g_audio_lock);
    return 1u;
}

void audio_irq_handler(void) {
    uint32_t irq_status;
    if (g_audio_bar == 0u) {
        return;
    }
    spinlock_lock(&g_audio_lock);
    irq_status = audio_read32(LAMP_AUDIO_REG_IRQ_STATUS);
    if ((irq_status & LAMP_AUDIO_IRQ_COMPLETION) != 0u) {
        audio_reap_locked();
    }
    if ((irq_status & LAMP_AUDIO_IRQ_ERROR) != 0u) {
        g_audio_errors++;
    }
    if (irq_status != 0u) {
        audio_write32(LAMP_AUDIO_REG_IRQ_ACK, irq_status);
    }
    spinlock_unlock(&g_audio_lock);
}

uint32_t audio_active(void) {
    return g_audio_active ? 1u : 0u;
}

uint32_t audio_completed_count(void) {
    return g_audio_completed;
}

uint32_t audio_error_count(void) {
    return g_audio_errors;
}
