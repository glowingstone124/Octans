#include "../include/kernel/console_fb.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/types.h"

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
static volatile uint32_t *const g_fb = (volatile uint32_t *)(uintptr_t)FB_BASE;

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
    console_fb_clear();
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
