#ifndef LAMP_KERNEL_CONSOLE_FB_H
#define LAMP_KERNEL_CONSOLE_FB_H

#include "types.h"

void console_fb_init(void);
void console_fb_putc(uint32_t c);
void console_fb_puts(const char *s);

void console_fb_set_colors(uint32_t fg, uint32_t bg);
void console_fb_clear(void);
void console_fb_set_text_output(uint32_t enabled);
uint32_t console_fb_text_output_enabled(void);

/* Pixel primitives used by the graphical VM screen. Callers serialize with
 * kio_lock() and submit one combined damage rectangle when drawing a frame. */
void console_fb_graphics_fill_rect(uint32_t x, uint32_t y,
                                   uint32_t width, uint32_t height,
                                   uint32_t color);
void console_fb_graphics_draw_text(uint32_t x, uint32_t y,
                                   const char *text, uint32_t color,
                                   uint32_t scale);
uint32_t console_fb_graphics_read_pixel(uint32_t x, uint32_t y);
void console_fb_graphics_write_pixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t console_fb_graphics_select_buffer(uint32_t offset);
uint32_t console_fb_graphics_page_flip(uint32_t offset);
void console_fb_graphics_present(uint32_t x, uint32_t y,
                                 uint32_t width, uint32_t height);

/* Switch the console from the firmware framebuffer to Lamp PCI GPU VRAM. */
uint32_t console_fb_attach_pci(uint32_t control_bar, uint32_t vram_bar);
void console_fb_detach_pci(void);
uint32_t console_fb_pci_active(void);

#endif
