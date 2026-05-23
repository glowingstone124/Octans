#ifndef LAMP_KERNEL_CONSOLE_FB_H
#define LAMP_KERNEL_CONSOLE_FB_H

#include "types.h"

void console_fb_init(void);
void console_fb_putc(uint32_t c);
void console_fb_puts(const char *s);

void console_fb_set_colors(uint32_t fg, uint32_t bg);
void console_fb_clear(void);

#endif
