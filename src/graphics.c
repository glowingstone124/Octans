#include "../include/kernel/graphics.h"
#include "../include/kernel/wm.h"

void graphics_init(void) {
    wm_init();
}

uint32_t graphics_active(void) {
    return wm_active();
}
