#ifndef LAMP_KERNEL_WM_H
#define LAMP_KERNEL_WM_H

#include "types.h"

#define WM_MAX_WINDOWS 8u
#define WM_MAX_LINES 5u

void wm_init(void);
void wm_start_compositor(void);
uint32_t wm_active(void);

uint32_t wm_window_create(const char *title,
                          uint32_t x, uint32_t y,
                          uint32_t width, uint32_t height,
                          uint32_t accent);
uint32_t wm_window_set_line(uint32_t id, uint32_t line,
                            const char *text, uint32_t color);
uint32_t wm_window_move(uint32_t id, uint32_t x, uint32_t y);
uint32_t wm_window_raise(uint32_t id);
uint32_t wm_window_set_visible(uint32_t id, uint32_t visible);
uint32_t wm_window_count(void);

/* PS/2 relative motion and the low three PS/2 button bits. */
void wm_pointer_event(int32_t dx, int32_t dy, uint32_t buttons);

#endif
