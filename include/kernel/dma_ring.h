#ifndef LAMP_KERNEL_DMA_RING_H
#define LAMP_KERNEL_DMA_RING_H

#include "types.h"
#include "../../../include/lampvm/device_abi.h"

typedef struct dma_ring {
    lamp_dma_desc_t *submissions;
    lamp_dma_completion_t *completions;
    uint32_t submission_count;
    uint32_t completion_count;
    uint32_t submission_iova;
    uint32_t completion_iova;
    uint32_t submission_tail;
    uint32_t completion_head;
} dma_ring_t;

uint32_t dma_ring_count_valid(uint32_t count);
uint32_t dma_ring_init(dma_ring_t *ring,
                       lamp_dma_desc_t *submissions, uint32_t submission_count,
                       lamp_dma_completion_t *completions, uint32_t completion_count);
uint32_t dma_ring_submit(dma_ring_t *ring, uint32_t device_head,
                         uint32_t buffer_pa, uint32_t length,
                         uint32_t flags, uint32_t cookie);
uint32_t dma_ring_reap(dma_ring_t *ring, uint32_t device_tail,
                       lamp_dma_completion_t *completion_out);

#endif
