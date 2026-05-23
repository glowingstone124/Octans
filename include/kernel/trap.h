#ifndef LAMP_KERNEL_TRAP_H
#define LAMP_KERNEL_TRAP_H

#include "types.h"

typedef void (*trap_handler_t)(uint32_t irq_no);

void trap_init(void);
void trap_dispatch(uint32_t irq_no);
void trap_register(uint32_t irq_no, trap_handler_t handler);

#endif
