#ifndef LAMP_KERNEL_CONSOLE_H
#define LAMP_KERNEL_CONSOLE_H

#include "types.h"

enum {
    CONSOLE_IO_OK = 0,
    CONSOLE_IO_BLOCKED = -1
};

enum {
    TTY_LFLAG_ECHO = (1u << 0),
    TTY_LFLAG_ICANON = (1u << 1),
    TTY_LFLAG_ISIG = (1u << 2)
};

void console_init(void);
void console_rx_feed(uint8_t c);
uint32_t console_rx_dropped(void);
uint32_t console_rx_lines(void);
uint32_t console_can_read(void);
int console_wait_readable(uint32_t timeout_ticks, uint32_t nonblock);

uint32_t console_tty_get_lflag(void);
uint32_t console_tty_set_lflag(uint32_t lflag);

int console_read(uint8_t *dst, uint32_t len, uint32_t nonblock);
uint32_t console_write(const uint8_t *src, uint32_t len);

#endif
