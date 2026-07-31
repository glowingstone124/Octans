#include "../include/kernel/dma_ring.h"

#include "../include/kernel/iommu.h"

static void dma_zero(void *ptr, uint32_t bytes) {
    uint8_t *p = (uint8_t *)ptr;
    for (uint32_t i = 0u; i < bytes; i++) {
        p[i] = 0u;
    }
}

uint32_t dma_ring_count_valid(uint32_t count) {
    return count >= LAMP_DMA_RING_MIN_COUNT &&
           count <= LAMP_DMA_RING_MAX_COUNT &&
           (count & (count - 1u)) == 0u;
}

uint32_t dma_ring_init(dma_ring_t *ring,
                       lamp_dma_desc_t *submissions, uint32_t submission_count,
                       lamp_dma_completion_t *completions, uint32_t completion_count) {
    uint32_t submission_iova;
    uint32_t completion_iova;
    if (!ring || !submissions || !completions ||
        !dma_ring_count_valid(submission_count) ||
        !dma_ring_count_valid(completion_count) ||
        !iommu_dma_iova((uint32_t)(uintptr_t)submissions,
                        submission_count * (uint32_t)sizeof(*submissions),
                        &submission_iova) ||
        !iommu_dma_iova((uint32_t)(uintptr_t)completions,
                        completion_count * (uint32_t)sizeof(*completions),
                        &completion_iova)) {
        return 0u;
    }

    dma_zero(submissions, submission_count * (uint32_t)sizeof(*submissions));
    dma_zero(completions, completion_count * (uint32_t)sizeof(*completions));
    ring->submissions = submissions;
    ring->completions = completions;
    ring->submission_count = submission_count;
    ring->completion_count = completion_count;
    ring->submission_iova = submission_iova;
    ring->completion_iova = completion_iova;
    ring->submission_tail = 0u;
    ring->completion_head = 0u;
    __asm__ volatile("" ::: "memory");
    return 1u;
}

uint32_t dma_ring_submit(dma_ring_t *ring, uint32_t device_head,
                         uint32_t buffer_pa, uint32_t length,
                         uint32_t flags, uint32_t cookie) {
    uint32_t next;
    uint32_t buffer_iova;
    lamp_dma_desc_t *desc;
    if (!ring || !ring->submissions || length == 0u ||
        device_head >= ring->submission_count ||
        !iommu_dma_iova(buffer_pa, length, &buffer_iova)) {
        return 0u;
    }
    next = (ring->submission_tail + 1u) & (ring->submission_count - 1u);
    if (next == device_head) {
        return 0u;
    }

    desc = &ring->submissions[ring->submission_tail];
    desc->addr_lo = buffer_iova;
    desc->addr_hi = 0u;
    desc->length = length;
    desc->flags = flags & (LAMP_DMA_DESC_F_IRQ | LAMP_DMA_DESC_F_END);
    desc->cookie = cookie;
    desc->reserved0 = 0u;
    desc->reserved1 = 0u;
    desc->reserved2 = 0u;
    __asm__ volatile("" ::: "memory");
    ring->submission_tail = next;
    return 1u;
}

uint32_t dma_ring_reap(dma_ring_t *ring, uint32_t device_tail,
                       lamp_dma_completion_t *completion_out) {
    if (!ring || !ring->completions || !completion_out ||
        device_tail >= ring->completion_count ||
        ring->completion_head == device_tail) {
        return 0u;
    }
    __asm__ volatile("" ::: "memory");
    *completion_out = ring->completions[ring->completion_head];
    ring->completion_head =
        (ring->completion_head + 1u) & (ring->completion_count - 1u);
    return 1u;
}
