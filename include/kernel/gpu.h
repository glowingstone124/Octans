#ifndef LAMP_KERNEL_GPU_H
#define LAMP_KERNEL_GPU_H

#include "types.h"

void gpu_init(void);
void gpu_irq_handler(void);
uint32_t gpu_active(void);
uint32_t gpu_cursor_available(void);
void gpu_cursor_update(uint32_t x, uint32_t y,
                       uint32_t buttons, uint32_t visible);

#endif
