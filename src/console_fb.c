#include "../include/kernel/console_fb.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/types.h"
#include "../../include/lampvm/device_abi.h"

#define CELL_W 8
#define CELL_H 8
#define COLS ((int)(FB_WIDTH / CELL_W))
#define ROWS ((int)(FB_HEIGHT / CELL_H))

#define ASCII_MIN 32
#define ASCII_MAX 126
#define FONT_PIXELS 64
#define FONT_GLYPHS (ASCII_MAX - ASCII_MIN + 1)
#define FG_DEFAULT 0x00FFFFFFu
#define BG_DEFAULT 0x00000000u

/* ANSI SGR state machine */
enum {
    ANSI_NORM = 0,
    ANSI_ESC  = 1,
    ANSI_CSI  = 2
};

/* 16 standard ANSI colors → 32-bit ARGB */
static const uint32_t g_ansi_palette[16] = {
    0xFF000000u, /* 0: Black        */
    0xFFAA0000u, /* 1: Red          */
    0xFF00AA00u, /* 2: Green        */
    0xFFAA5500u, /* 3: Yellow/Brown */
    0xFF0000AAu, /* 4: Blue         */
    0xFFAA00AAu, /* 5: Magenta      */
    0xFF00AAAAu, /* 6: Cyan         */
    0xFFAAAAAAu, /* 7: Light Gray   */
    0xFF555555u, /* 8: Dark Gray    */
    0xFFFF5555u, /* 9: Bright Red   */
    0xFF55FF55u, /*10: Bright Green */
    0xFFFFFF55u, /*11: Bright Yellow*/
    0xFF5555FFu, /*12: Bright Blue  */
    0xFFFF55FFu, /*13: Bright Magenta*/
    0xFF55FFFFu, /*14: Bright Cyan  */
    0xFFFFFFFFu, /*15: White        */
};

static int g_cursor_x;
static int g_cursor_y;
static uint32_t g_fg = FG_DEFAULT;
static uint32_t g_bg = BG_DEFAULT;
static int g_ansi_state = ANSI_NORM;
static int g_ansi_bold;
static uint32_t g_ansi_accum[8];
static uint32_t g_ansi_naccum;
static volatile uint32_t *g_fb = (volatile uint32_t *)(uintptr_t)FB_BASE;
static uint32_t g_gpu_control_bar;
static uint32_t g_gpu_vram_bar;
static uint32_t g_gpu_draw_offset;
static uint32_t g_gpu_scanout_offset;
static uint32_t g_gpu_active;
static uint32_t g_text_output = 1u;

/*
 * Imported from toolchain example ascii_fb.c, values are 0/1 pixels.
 * Layout: glyph-major, each glyph is 8x8 = 64 entries.
 */
static int g_font[FONT_GLYPHS * FONT_PIXELS] = {
#include "console_font_8x8_data.inc"
};

static inline void fb_accel_out32(uint32_t addr, uint32_t value) {
    __asm__ volatile("out %0, %1" :: "r"(value), "r"(addr));
}

static inline uint32_t gpu_read32(uint32_t reg) {
    return *(volatile uint32_t *)(uintptr_t)(g_gpu_control_bar + reg);
}

static inline void gpu_write32(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)(g_gpu_control_bar + reg) = value;
}

static void gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g_gpu_active || w == 0u || h == 0u) {
        return;
    }
    gpu_write32(LAMP_GPU_REG_DAMAGE_X, x);
    gpu_write32(LAMP_GPU_REG_DAMAGE_Y, y);
    gpu_write32(LAMP_GPU_REG_DAMAGE_W, w);
    gpu_write32(LAMP_GPU_REG_DAMAGE_H, h);
    gpu_write32(LAMP_GPU_REG_COMMAND, LAMP_GPU_CMD_FLUSH);
}

static void clear_cell(int cx, int cy) {
    int py = cy * CELL_H;
    int px = cx * CELL_W;
    for (int y = 0; y < CELL_H; y++) {
        for (int x = 0; x < CELL_W; x++) {
            g_fb[(py + y) * (int)FB_WIDTH + (px + x)] = g_bg;
        }
    }
}

static void draw_box(int cx, int cy) {
    int py = cy * CELL_H;
    int px = cx * CELL_W;
    for (int y = 0; y < CELL_H; y++) {
        for (int x = 0; x < CELL_W; x++) {
            if (y == 0 || y == (CELL_H - 1) || x == 0 || x == (CELL_W - 1)) {
                g_fb[(py + y) * (int)FB_WIDTH + (px + x)] = g_fg;
            } else {
                g_fb[(py + y) * (int)FB_WIDTH + (px + x)] = g_bg;
            }
        }
    }
}

static void draw_char(int ch, int cx, int cy) {
    int py = cy * CELL_H;
    int px = cx * CELL_W;
    if (ch < ASCII_MIN || ch > ASCII_MAX) {
        draw_box(cx, cy);
        return;
    }

    int glyph_base = (ch - ASCII_MIN) * FONT_PIXELS;
    for (int row = 0; row < CELL_H; row++) {
        int row_base = glyph_base + row * CELL_W;
        for (int col = 0; col < CELL_W; col++) {
            int v = g_font[row_base + col];
            if (v) {
                g_fb[(py + row) * (int)FB_WIDTH + (px + col)] = g_fg;
            } else {
                g_fb[(py + row) * (int)FB_WIDTH + (px + col)] = g_bg;
            }
        }
    }
}

static void scroll_one_line(void) {
    if (g_gpu_active) {
        const uint32_t retained_rows = FB_HEIGHT - CELL_H;
        for (uint32_t y = 0u; y < retained_rows; y++) {
            const uint32_t src = (y + CELL_H) * FB_WIDTH;
            const uint32_t dst = y * FB_WIDTH;
            for (uint32_t x = 0u; x < FB_WIDTH; x++) {
                g_fb[dst + x] = g_fb[src + x];
            }
        }
        for (uint32_t y = retained_rows; y < FB_HEIGHT; y++) {
            const uint32_t row = y * FB_WIDTH;
            for (uint32_t x = 0u; x < FB_WIDTH; x++) {
                g_fb[row + x] = g_bg;
            }
        }
        gpu_flush(0u, 0u, FB_WIDTH, FB_HEIGHT);
        return;
    }
    fb_accel_out32(IO_FB_ACCEL_ARG0, g_bg);
    fb_accel_out32(IO_FB_ACCEL_CMD, FB_ACCEL_CMD_SCROLL_UP_8PX);
}

static void newline(void) {
    g_cursor_x = 0;
    g_cursor_y++;
    if (g_cursor_y >= ROWS) {
        scroll_one_line();
        g_cursor_y = ROWS - 1;
    }
}

void console_fb_clear(void) {
    if (g_gpu_active) {
        for (uint32_t i = 0u; i < FB_WIDTH * FB_HEIGHT; i++) {
            g_fb[i] = g_bg;
        }
        gpu_flush(0u, 0u, FB_WIDTH, FB_HEIGHT);
        g_cursor_x = 0;
        g_cursor_y = 0;
        return;
    }
    fb_accel_out32(IO_FB_ACCEL_ARG0, g_bg);
    fb_accel_out32(IO_FB_ACCEL_CMD, FB_ACCEL_CMD_CLEAR);
    g_cursor_x = 0;
    g_cursor_y = 0;
}

static void ansi_sgr_apply(uint32_t code) {
    /* bright palette offset for bold colors */
    uint32_t off = g_ansi_bold ? 8u : 0u;
    if (code == 0u) {
        g_fg = FG_DEFAULT; g_bg = BG_DEFAULT; g_ansi_bold = 0;
    } else if (code == 1u) {
        g_ansi_bold = 1;
    } else if (code >= 30u && code <= 37u) {
        g_fg = g_ansi_palette[(code - 30u) + off];
    } else if (code >= 40u && code <= 47u) {
        g_bg = g_ansi_palette[(code - 40u) + off];
    }
}

void console_fb_putc(uint32_t c) {
    if (!g_text_output) {
        return;
    }
    /* ---- ANSI SGR escape sequence parser ---- */
    if (g_ansi_state == ANSI_ESC) {
        if (c == '[') {
            g_ansi_state = ANSI_CSI;
            g_ansi_naccum = 0u;
            g_ansi_accum[0] = 0u;
            return;
        }
        g_ansi_state = ANSI_NORM; return;
    }
    if (g_ansi_state == ANSI_CSI) {
        if (c >= '0' && c <= '9') {
            g_ansi_accum[g_ansi_naccum] = g_ansi_accum[g_ansi_naccum] * 10u + (c - '0');
            return;
        }
        if (c == ';') {
            if (g_ansi_naccum + 1u < 7u) {
                g_ansi_naccum++;
                g_ansi_accum[g_ansi_naccum] = 0u;
            }
            return;
        }
        if (c == 'm') {
            for (uint32_t i = 0u; i <= g_ansi_naccum; i++) {
                ansi_sgr_apply(g_ansi_accum[i]);
            }
        }
        g_ansi_state = ANSI_NORM; return;
    }
    if (c == '\033') { g_ansi_state = ANSI_ESC; return; }
    /* ---- end ANSI parser ---- */
    if (c == (uint32_t)'\a') {
        return;
    }
    if (c == (uint32_t)'\n') {
        newline();
        return;
    }
    if (c == (uint32_t)'\v') {
        newline();
        return;
    }
    if (c == (uint32_t)'\f') {
        console_fb_clear();
        return;
    }
    if (c == (uint32_t)'\b') {
        if (g_cursor_x > 0) {
            g_cursor_x--;
        } else if (g_cursor_y > 0) {
            g_cursor_y--;
            g_cursor_x = COLS - 1;
        } else {
            return;
        }
        clear_cell(g_cursor_x, g_cursor_y);
        gpu_flush((uint32_t)g_cursor_x * CELL_W,
                  (uint32_t)g_cursor_y * CELL_H, CELL_W, CELL_H);
        return;
    }
    if (c == (uint32_t)'\r') {
        g_cursor_x = 0;
        return;
    }
    if (c == (uint32_t)'\t') {
        for (int i = 0; i < 4; i++) {
            console_fb_putc((uint32_t)' ');
        }
        return;
    }
    if (c == 0u) {
        clear_cell(g_cursor_x, g_cursor_y);
    } else {
        draw_char((int)c, g_cursor_x, g_cursor_y);
    }

    gpu_flush((uint32_t)g_cursor_x * CELL_W,
              (uint32_t)g_cursor_y * CELL_H, CELL_W, CELL_H);

    g_cursor_x++;
    if (g_cursor_x >= COLS) {
        newline();
    }
}

void console_fb_init(void) {
    g_fg = FG_DEFAULT;
    g_bg = BG_DEFAULT;
    g_cursor_x = 0;
    g_cursor_y = 0;
    g_gpu_control_bar = 0u;
    g_gpu_vram_bar = 0u;
    g_gpu_draw_offset = 0u;
    g_gpu_scanout_offset = 0u;
    g_gpu_active = 0u;
    g_text_output = 1u;
    g_fb = (volatile uint32_t *)(uintptr_t)FB_BASE;
    console_fb_clear();
}

uint32_t console_fb_attach_pci(uint32_t control_bar, uint32_t vram_bar) {
    volatile uint32_t *const firmware_fb =
        (volatile uint32_t *)(uintptr_t)FB_BASE;
    volatile uint32_t *const pci_fb =
        (volatile uint32_t *)(uintptr_t)vram_bar;

    if (control_bar == 0u || vram_bar == 0u || g_gpu_active) {
        return 0u;
    }

    for (uint32_t i = 0u; i < FB_WIDTH * FB_HEIGHT; i++) {
        pci_fb[i] = firmware_fb[i];
    }

    g_gpu_control_bar = control_bar;
    gpu_write32(LAMP_GPU_REG_PENDING_OFFSET, 0u);
    gpu_write32(LAMP_GPU_REG_DAMAGE_X, 0u);
    gpu_write32(LAMP_GPU_REG_DAMAGE_Y, 0u);
    gpu_write32(LAMP_GPU_REG_DAMAGE_W, FB_WIDTH);
    gpu_write32(LAMP_GPU_REG_DAMAGE_H, FB_HEIGHT);
    gpu_write32(LAMP_GPU_REG_IRQ_ENABLE,
                LAMP_GPU_IRQ_FLIP_COMPLETE | LAMP_GPU_IRQ_ERROR);
    gpu_write32(LAMP_GPU_REG_COMMAND,
                LAMP_GPU_CMD_ENABLE | LAMP_GPU_CMD_PAGE_FLIP);

    if ((gpu_read32(LAMP_GPU_REG_STATUS) &
         (LAMP_GPU_STATUS_ENABLED | LAMP_GPU_STATUS_BAD_SCANOUT |
          LAMP_GPU_STATUS_BAD_COMMAND)) != LAMP_GPU_STATUS_ENABLED) {
        gpu_write32(LAMP_GPU_REG_COMMAND, LAMP_GPU_CMD_DISABLE);
        gpu_write32(LAMP_GPU_REG_IRQ_ENABLE, 0u);
        g_gpu_control_bar = 0u;
        return 0u;
    }

    g_fb = pci_fb;
    g_gpu_vram_bar = vram_bar;
    g_gpu_draw_offset = 0u;
    g_gpu_scanout_offset = 0u;
    g_gpu_active = 1u;
    return 1u;
}

void console_fb_detach_pci(void) {
    if (!g_gpu_active) {
        return;
    }
    /* DISABLE restores the device's saved firmware scanout synchronously. */
    gpu_write32(LAMP_GPU_REG_IRQ_ENABLE, 0u);
    gpu_write32(LAMP_GPU_REG_COMMAND, LAMP_GPU_CMD_DISABLE);
    g_fb = (volatile uint32_t *)(uintptr_t)FB_BASE;
    g_gpu_active = 0u;
    g_gpu_control_bar = 0u;
    g_gpu_vram_bar = 0u;
    g_gpu_draw_offset = 0u;
    g_gpu_scanout_offset = 0u;
}

uint32_t console_fb_pci_active(void) {
    return g_gpu_active ? 1u : 0u;
}

void console_fb_set_text_output(uint32_t enabled) {
    g_text_output = enabled ? 1u : 0u;
}

uint32_t console_fb_text_output_enabled(void) {
    return g_text_output ? 1u : 0u;
}

void console_fb_graphics_fill_rect(uint32_t x, uint32_t y,
                                   uint32_t width, uint32_t height,
                                   uint32_t color) {
    if (x >= FB_WIDTH || y >= FB_HEIGHT || width == 0u || height == 0u) {
        return;
    }
    if (width > FB_WIDTH - x) {
        width = FB_WIDTH - x;
    }
    if (height > FB_HEIGHT - y) {
        height = FB_HEIGHT - y;
    }
    for (uint32_t row = y; row < y + height; row++) {
        const uint32_t base = row * FB_WIDTH + x;
        for (uint32_t col = 0u; col < width; col++) {
            g_fb[base + col] = color;
        }
    }
}

void console_fb_graphics_draw_text(uint32_t x, uint32_t y,
                                   const char *text, uint32_t color,
                                   uint32_t scale) {
    uint32_t pen_x = x;
    if (!text || scale == 0u || scale > 4u || y >= FB_HEIGHT) {
        return;
    }
    while (*text != '\0') {
        uint32_t ch = (uint32_t)(uint8_t)*text++;
        if (ch == (uint32_t)'\n') {
            pen_x = x;
            y += CELL_H * scale;
            continue;
        }
        if (pen_x + CELL_W * scale > FB_WIDTH ||
            y + CELL_H * scale > FB_HEIGHT) {
            break;
        }
        if (ch < ASCII_MIN || ch > ASCII_MAX) {
            ch = (uint32_t)'?';
        }
        const uint32_t glyph_base = (ch - ASCII_MIN) * FONT_PIXELS;
        for (uint32_t row = 0u; row < CELL_H; row++) {
            for (uint32_t col = 0u; col < CELL_W; col++) {
                if (g_font[glyph_base + row * CELL_W + col] == 0) {
                    continue;
                }
                for (uint32_t sy = 0u; sy < scale; sy++) {
                    const uint32_t pixel_row = (y + row * scale + sy) * FB_WIDTH;
                    for (uint32_t sx = 0u; sx < scale; sx++) {
                        g_fb[pixel_row + pen_x + col * scale + sx] = color;
                    }
                }
            }
        }
        pen_x += CELL_W * scale;
    }
}

uint32_t console_fb_graphics_read_pixel(uint32_t x, uint32_t y) {
    if (x >= FB_WIDTH || y >= FB_HEIGHT) {
        return 0u;
    }
    return g_fb[y * FB_WIDTH + x];
}

void console_fb_graphics_write_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= FB_WIDTH || y >= FB_HEIGHT) {
        return;
    }
    g_fb[y * FB_WIDTH + x] = color;
}

uint32_t console_fb_graphics_select_buffer(uint32_t offset) {
    if (!g_gpu_active) {
        return offset == 0u ? 1u : 0u;
    }
    if ((offset & 3u) != 0u || offset > LAMP_GPU_VRAM_SIZE ||
        FB_SIZE > LAMP_GPU_VRAM_SIZE - offset) {
        return 0u;
    }
    g_fb = (volatile uint32_t *)(uintptr_t)(g_gpu_vram_bar + offset);
    g_gpu_draw_offset = offset;
    return 1u;
}

uint32_t console_fb_graphics_page_flip(uint32_t offset) {
    uint32_t status;
    if (!g_gpu_active) {
        return offset == 0u ? 1u : 0u;
    }
    if (offset != g_gpu_draw_offset) {
        return 0u;
    }
    gpu_write32(LAMP_GPU_REG_PENDING_OFFSET, offset);
    gpu_write32(LAMP_GPU_REG_COMMAND,
                LAMP_GPU_CMD_ENABLE | LAMP_GPU_CMD_PAGE_FLIP);
    status = gpu_read32(LAMP_GPU_REG_STATUS);
    if ((status & (LAMP_GPU_STATUS_ENABLED | LAMP_GPU_STATUS_BAD_SCANOUT |
                   LAMP_GPU_STATUS_BAD_COMMAND)) != LAMP_GPU_STATUS_ENABLED ||
        gpu_read32(LAMP_GPU_REG_SCANOUT_OFFSET) != offset) {
        g_fb = (volatile uint32_t *)(uintptr_t)
            (g_gpu_vram_bar + g_gpu_scanout_offset);
        g_gpu_draw_offset = g_gpu_scanout_offset;
        return 0u;
    }
    g_gpu_scanout_offset = offset;
    return 1u;
}

void console_fb_graphics_present(uint32_t x, uint32_t y,
                                 uint32_t width, uint32_t height) {
    if (x >= FB_WIDTH || y >= FB_HEIGHT || width == 0u || height == 0u) {
        return;
    }
    if (width > FB_WIDTH - x) {
        width = FB_WIDTH - x;
    }
    if (height > FB_HEIGHT - y) {
        height = FB_HEIGHT - y;
    }
    if (g_gpu_active && g_gpu_draw_offset != g_gpu_scanout_offset) {
        return;
    }
    gpu_flush(x, y, width, height);
}

void console_fb_set_colors(uint32_t fg, uint32_t bg) {
    g_fg = fg;
    g_bg = bg;
}

void console_fb_puts(const char *s) {
    if (!s) {
        return;
    }
    const uint8_t *p = (const uint8_t *)s;
    while (*p != 0u) {
        console_fb_putc((uint32_t)*p);
        p++;
    }
}
