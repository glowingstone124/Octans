#include "../include/kernel/wm.h"

#include "../include/kernel/audio.h"
#include "../include/kernel/console_fb.h"
#include "../include/kernel/gpu.h"
#include "../include/kernel/pci.h"
#include "../include/kernel/platform.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/sched.h"
#include "../../include/lampvm/device_abi.h"

#define WM_TAG "wm"
#define WM_TITLE_MAX 24u
#define WM_LINE_MAX 48u
#define WM_TITLEBAR_HEIGHT 30u
#define WM_CURSOR_WIDTH 12u
#define WM_CURSOR_HEIGHT 18u
#define WM_CLICK_PENDING (1u << 31)
#define WM_CLICK_X_MASK 0x3FFu
#define WM_CLICK_Y_MASK 0x1FFu
#define WM_CLICK_Y_SHIFT 10u

#define COLOR_DESKTOP 0x00091420u
#define COLOR_DESKTOP_ALT 0x000C1929u
#define COLOR_PANEL 0x00121F31u
#define COLOR_WINDOW 0x00172538u
#define COLOR_WINDOW_TOP 0x001D3047u
#define COLOR_WINDOW_FOCUS 0x00213D57u
#define COLOR_BORDER 0x00324A64u
#define COLOR_SHADOW 0x00050A12u
#define COLOR_ACCENT 0x0038BDF8u
#define COLOR_TEXT 0x00E8F1FAu
#define COLOR_MUTED 0x0094A8BEu
#define COLOR_OK 0x0034D399u
#define COLOR_WARN 0x00FBBF24u
#define COLOR_CURSOR_OUTLINE 0x00030A10u
#define COLOR_CURSOR_FILL 0x00F8FAFCu
#define COLOR_CURSOR_ACTIVE 0x0038BDF8u

typedef struct wm_window {
    uint32_t id;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t accent;
    uint32_t visible;
    char title[WM_TITLE_MAX];
    char lines[WM_MAX_LINES][WM_LINE_MAX];
    uint32_t line_colors[WM_MAX_LINES];
} wm_window_t;

static wm_window_t g_windows[WM_MAX_WINDOWS];
static uint8_t g_z_order[WM_MAX_WINDOWS];
static uint32_t g_window_count;
static uint32_t g_next_window_id;
static volatile uint32_t g_wm_active;
static uint32_t g_double_buffered;
static uint32_t g_hardware_cursor;
static uint32_t g_scanout_offset;
static volatile uint32_t g_redraw_pending;
static volatile uint32_t g_click_event;
static uint32_t g_compositor_started;

/* The mouse IRQ is the sole writer. Keep the hardware cursor coordinates
 * independent from the compositor's long-lived kio lock. */
static volatile int32_t g_pointer_x;
static volatile int32_t g_pointer_y;
static volatile uint32_t g_pointer_buttons;
static uint32_t g_pointer_under[WM_CURSOR_WIDTH * WM_CURSOR_HEIGHT];
static uint32_t g_pointer_saved;

static uint32_t wm_hit_test_locked(uint32_t x, uint32_t y);
static void wm_raise_index_locked(uint32_t index);

static inline uint32_t wm_atomic_exchange_u32(volatile uint32_t *ptr,
                                               uint32_t value) {
    uint32_t old;
    __asm__ volatile (
        "xchg %0, %1, %2, 0\n"
        : "=&r"(old)
        : "r"(ptr), "r"(value)
        : "memory"
    );
    return old;
}

static void wm_queue_click_from_irq(uint32_t x, uint32_t y) {
    const uint32_t event = WM_CLICK_PENDING |
        (x & WM_CLICK_X_MASK) |
        ((y & WM_CLICK_Y_MASK) << WM_CLICK_Y_SHIFT);

    /* The pointer IRQ may interrupt code that owns kio_lock. Publishing one
     * packed word keeps the hard-IRQ path non-blocking; the compositor claims
     * it with XCHG. A second click may replace an unconsumed first click, which
     * is preferable to ever stalling pointer motion in an IRQ. */
    (void)wm_atomic_exchange_u32(&g_click_event, event);
}

static const char *const g_pointer_shape[WM_CURSOR_HEIGHT] = {
    "X...........",
    "XX..........",
    "XOX.........",
    "XOOX........",
    "XOOOX.......",
    "XOOOOX......",
    "XOOOOOX.....",
    "XOOOOOOX....",
    "XOOOOOOOX...",
    "XOOOOOOOOX..",
    "XOOOOXXXXX..",
    "XOOXOX......",
    "XOX.XOX.....",
    "XX..XOX.....",
    "X....XOX....",
    ".....XOX....",
    ".....XOX....",
    ".....XXX...."
};

static void wm_copy_text(char *dst, uint32_t capacity, const char *src) {
    uint32_t i = 0u;
    if (!dst || capacity == 0u) {
        return;
    }
    if (src) {
        while (i + 1u < capacity && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static wm_window_t *wm_find_window(uint32_t id, uint32_t *index_out) {
    for (uint32_t i = 0u; i < g_window_count; i++) {
        if (g_windows[i].id == id) {
            if (index_out) {
                *index_out = i;
            }
            return &g_windows[i];
        }
    }
    return (wm_window_t *)0;
}

static void wm_border(uint32_t x, uint32_t y,
                      uint32_t width, uint32_t height,
                      uint32_t color) {
    if (width < 2u || height < 2u) {
        return;
    }
    console_fb_graphics_fill_rect(x, y, width, 1u, color);
    console_fb_graphics_fill_rect(x, y + height - 1u, width, 1u, color);
    console_fb_graphics_fill_rect(x, y, 1u, height, color);
    console_fb_graphics_fill_rect(x + width - 1u, y, 1u, height, color);
}

static void wm_draw_text_clipped(uint32_t x, uint32_t y, uint32_t width,
                                 const char *text, uint32_t color,
                                 uint32_t scale) {
    char clipped[WM_LINE_MAX];
    uint32_t max_chars;
    uint32_t i = 0u;
    if (!text || scale == 0u || width < 8u * scale) {
        return;
    }
    max_chars = width / (8u * scale);
    if (max_chars >= WM_LINE_MAX) {
        max_chars = WM_LINE_MAX - 1u;
    }
    while (i < max_chars && text[i] != '\0') {
        clipped[i] = text[i];
        i++;
    }
    clipped[i] = '\0';
    console_fb_graphics_draw_text(x, y, clipped, color, scale);
}

static void wm_draw_desktop(void) {
    console_fb_graphics_fill_rect(0u, 0u, FB_WIDTH, FB_HEIGHT, COLOR_DESKTOP);
    console_fb_graphics_fill_rect(0u, 0u, FB_WIDTH, 36u, COLOR_PANEL);
    console_fb_graphics_fill_rect(0u, 36u, FB_WIDTH, 1u, COLOR_BORDER);
    console_fb_graphics_fill_rect(0u, FB_HEIGHT - 30u, FB_WIDTH, 30u, COLOR_PANEL);
    console_fb_graphics_fill_rect(0u, FB_HEIGHT - 31u, FB_WIDTH, 1u, COLOR_BORDER);
    console_fb_graphics_fill_rect(0u, 37u, FB_WIDTH, 26u, COLOR_DESKTOP_ALT);

    console_fb_graphics_draw_text(18u, 10u, "Octans", COLOR_TEXT, 2u);
    console_fb_graphics_draw_text(484u, 14u, "WORKSPACE 1", COLOR_ACCENT, 1u);
    console_fb_graphics_draw_text(18u, FB_HEIGHT - 20u,
                                  "SERIAL: SHELL", COLOR_MUTED, 1u);
    console_fb_graphics_fill_rect(535u, FB_HEIGHT - 21u, 8u, 8u, COLOR_OK);
    console_fb_graphics_draw_text(551u, FB_HEIGHT - 22u,
                                  "PS/2", COLOR_MUTED, 1u);
}

static void wm_draw_window(const wm_window_t *window, uint32_t focused) {
    uint32_t title_color;
    uint32_t max_lines;
    if (!window || !window->visible) {
        return;
    }
    title_color = focused ? COLOR_WINDOW_FOCUS : COLOR_WINDOW_TOP;
    console_fb_graphics_fill_rect(window->x + 6u, window->y + 7u,
                                  window->width, window->height, COLOR_SHADOW);
    console_fb_graphics_fill_rect(window->x, window->y,
                                  window->width, window->height, COLOR_WINDOW);
    console_fb_graphics_fill_rect(window->x, window->y,
                                  window->width, WM_TITLEBAR_HEIGHT, title_color);
    console_fb_graphics_fill_rect(window->x, window->y, 4u,
                                  WM_TITLEBAR_HEIGHT, window->accent);
    wm_border(window->x, window->y, window->width, window->height,
              focused ? window->accent : COLOR_BORDER);

    wm_draw_text_clipped(window->x + 14u, window->y + 10u,
                         window->width - 78u, window->title, COLOR_TEXT, 1u);
    console_fb_graphics_fill_rect(window->x + window->width - 52u,
                                  window->y + 10u, 8u, 8u, COLOR_WARN);
    console_fb_graphics_fill_rect(window->x + window->width - 30u,
                                  window->y + 10u, 8u, 8u, window->accent);

    max_lines = (window->height - WM_TITLEBAR_HEIGHT - 16u) / 19u;
    if (max_lines > WM_MAX_LINES) {
        max_lines = WM_MAX_LINES;
    }
    for (uint32_t line = 0u; line < max_lines; line++) {
        if (window->lines[line][0] == '\0') {
            continue;
        }
        wm_draw_text_clipped(window->x + 16u,
                             window->y + WM_TITLEBAR_HEIGHT + 15u + line * 19u,
                             window->width - 32u, window->lines[line],
                             window->line_colors[line], 1u);
    }
}

static uint32_t wm_pointer_pixel(uint32_t x, uint32_t y) {
    return g_pointer_shape[y][x] != '.' ? 1u : 0u;
}

static void wm_pointer_restore_locked(void) {
    if (!g_pointer_saved) {
        return;
    }
    for (uint32_t y = 0u; y < WM_CURSOR_HEIGHT; y++) {
        for (uint32_t x = 0u; x < WM_CURSOR_WIDTH; x++) {
            const int32_t px = g_pointer_x + (int32_t)x;
            const int32_t py = g_pointer_y + (int32_t)y;
            if (!wm_pointer_pixel(x, y) || px < 0 || py < 0 ||
                px >= (int32_t)FB_WIDTH || py >= (int32_t)FB_HEIGHT) {
                continue;
            }
            console_fb_graphics_write_pixel((uint32_t)px, (uint32_t)py,
                g_pointer_under[y * WM_CURSOR_WIDTH + x]);
        }
    }
    g_pointer_saved = 0u;
}

static void wm_pointer_draw_locked(void) {
    const uint32_t fill = (g_pointer_buttons & 1u) != 0u ?
        COLOR_CURSOR_ACTIVE : COLOR_CURSOR_FILL;
    for (uint32_t y = 0u; y < WM_CURSOR_HEIGHT; y++) {
        for (uint32_t x = 0u; x < WM_CURSOR_WIDTH; x++) {
            const int32_t px = g_pointer_x + (int32_t)x;
            const int32_t py = g_pointer_y + (int32_t)y;
            const char shape = g_pointer_shape[y][x];
            if (shape == '.' || px < 0 || py < 0 ||
                px >= (int32_t)FB_WIDTH || py >= (int32_t)FB_HEIGHT) {
                continue;
            }
            g_pointer_under[y * WM_CURSOR_WIDTH + x] =
                console_fb_graphics_read_pixel((uint32_t)px, (uint32_t)py);
            console_fb_graphics_write_pixel((uint32_t)px, (uint32_t)py,
                shape == 'X' ? COLOR_CURSOR_OUTLINE : fill);
        }
    }
    g_pointer_saved = 1u;
}

static void wm_pointer_present_locked(uint32_t old_x, uint32_t old_y) {
    uint32_t x0 = old_x < (uint32_t)g_pointer_x ? old_x : (uint32_t)g_pointer_x;
    uint32_t y0 = old_y < (uint32_t)g_pointer_y ? old_y : (uint32_t)g_pointer_y;
    uint32_t x1 = old_x + WM_CURSOR_WIDTH;
    uint32_t y1 = old_y + WM_CURSOR_HEIGHT;
    const uint32_t new_x1 = (uint32_t)g_pointer_x + WM_CURSOR_WIDTH;
    const uint32_t new_y1 = (uint32_t)g_pointer_y + WM_CURSOR_HEIGHT;
    if (new_x1 > x1) x1 = new_x1;
    if (new_y1 > y1) y1 = new_y1;
    if (x1 > FB_WIDTH) x1 = FB_WIDTH;
    if (y1 > FB_HEIGHT) y1 = FB_HEIGHT;
    console_fb_graphics_present(x0, y0, x1 - x0, y1 - y0);
}

static void wm_paint_scene_locked(void) {
    wm_draw_desktop();
    for (uint32_t z = 0u; z < g_window_count; z++) {
        const uint32_t index = g_z_order[z];
        wm_draw_window(&g_windows[index], z + 1u == g_window_count);
    }
}

static void wm_render_locked(void) {
    uint32_t target = 0u;
    if (g_hardware_cursor && !gpu_cursor_available()) {
        g_hardware_cursor = 0u;
        g_pointer_saved = 0u;
    }
    if (g_double_buffered) {
        target = g_scanout_offset == 0u ? FB_SIZE : 0u;
    }
    if (!g_hardware_cursor) {
        g_pointer_saved = 0u;
    }
    if (!console_fb_graphics_select_buffer(target)) {
        target = 0u;
        g_double_buffered = 0u;
        (void)console_fb_graphics_select_buffer(0u);
    }
    wm_paint_scene_locked();
    if (g_double_buffered) {
        if (console_fb_graphics_page_flip(target)) {
            g_scanout_offset = target;
        } else {
            target = g_scanout_offset;
            (void)console_fb_graphics_select_buffer(target);
            wm_paint_scene_locked();
            console_fb_graphics_present(0u, 0u, FB_WIDTH, FB_HEIGHT);
        }
    } else {
        console_fb_graphics_present(0u, 0u, FB_WIDTH, FB_HEIGHT);
    }
    if (!g_hardware_cursor) {
        wm_pointer_draw_locked();
        console_fb_graphics_present((uint32_t)g_pointer_x,
                                    (uint32_t)g_pointer_y,
                                    WM_CURSOR_WIDTH, WM_CURSOR_HEIGHT);
    }
}

static void wm_request_redraw_locked(void) {
    /* Protected by kio_lock(), which also serializes scene state. */
    g_redraw_pending = 1u;
}

static void wm_compositor_task(sched_task_t *task, void *arg) {
    (void)task;
    (void)arg;
    for (;;) {
        const uint32_t click =
            wm_atomic_exchange_u32(&g_click_event, 0u);
        uint32_t redraw;
        kio_lock();
        redraw = g_redraw_pending;
        g_redraw_pending = 0u;
        if ((click & WM_CLICK_PENDING) != 0u) {
            const uint32_t x = click & WM_CLICK_X_MASK;
            const uint32_t y =
                (click >> WM_CLICK_Y_SHIFT) & WM_CLICK_Y_MASK;
            const uint32_t hit = wm_hit_test_locked(x, y);
            if (hit != 0u && g_z_order[g_window_count - 1u] != hit - 1u) {
                wm_raise_index_locked(hit - 1u);
                redraw = 1u;
            }
        }
        if (redraw != 0u) {
            if (g_wm_active) {
                wm_render_locked();
            }
            kio_unlock();
            continue;
        }
        kio_unlock();
        /* One tick is also the lost-wakeup bound: pointer IRQs never block on
         * scheduler internals merely to request a visual focus change. */
        sched_sleep_ticks(1u);
    }
}

static uint32_t wm_window_create_locked(const char *title,
                                        uint32_t x, uint32_t y,
                                        uint32_t width, uint32_t height,
                                        uint32_t accent) {
    wm_window_t *window;
    uint32_t index;
    if (g_window_count >= WM_MAX_WINDOWS) {
        return 0u;
    }
    if (width < 160u) width = 160u;
    if (height < 90u) height = 90u;
    if (width > FB_WIDTH - 16u) width = FB_WIDTH - 16u;
    if (height > FB_HEIGHT - 52u) height = FB_HEIGHT - 52u;
    if (x > FB_WIDTH - width - 8u) x = FB_WIDTH - width - 8u;
    if (y < 44u) y = 44u;
    if (y > FB_HEIGHT - height - 36u) y = FB_HEIGHT - height - 36u;

    index = g_window_count++;
    window = &g_windows[index];
    window->id = g_next_window_id++;
    window->x = x;
    window->y = y;
    window->width = width;
    window->height = height;
    window->accent = accent;
    window->visible = 1u;
    wm_copy_text(window->title, WM_TITLE_MAX, title);
    for (uint32_t line = 0u; line < WM_MAX_LINES; line++) {
        window->lines[line][0] = '\0';
        window->line_colors[line] = COLOR_MUTED;
    }
    g_z_order[index] = (uint8_t)index;
    return window->id;
}

static void wm_raise_index_locked(uint32_t index) {
    uint32_t position = g_window_count;
    for (uint32_t z = 0u; z < g_window_count; z++) {
        if (g_z_order[z] == index) {
            position = z;
            break;
        }
    }
    if (position >= g_window_count || position + 1u == g_window_count) {
        return;
    }
    for (uint32_t z = position; z + 1u < g_window_count; z++) {
        g_z_order[z] = g_z_order[z + 1u];
    }
    g_z_order[g_window_count - 1u] = (uint8_t)index;
}

static uint32_t wm_hit_test_locked(uint32_t x, uint32_t y) {
    for (uint32_t z = g_window_count; z > 0u; z--) {
        const uint32_t index = g_z_order[z - 1u];
        const wm_window_t *window = &g_windows[index];
        if (window->visible && x >= window->x && y >= window->y &&
            x < window->x + window->width &&
            y < window->y + window->height) {
            return index + 1u;
        }
    }
    return 0u;
}

void wm_init(void) {
    uint32_t system_id;
    uint32_t devices_id;
    uint32_t console_id;

    g_window_count = 0u;
    g_next_window_id = 1u;
    g_wm_active = 0u;
    g_scanout_offset = 0u;
    g_double_buffered = console_fb_pci_active() &&
        LAMP_GPU_VRAM_SIZE >= FB_SIZE * 2u;
    g_hardware_cursor = gpu_cursor_available();
    g_redraw_pending = 0u;
    g_click_event = 0u;
    g_compositor_started = 0u;
    g_pointer_x = (int32_t)(FB_WIDTH / 2u);
    g_pointer_y = (int32_t)(FB_HEIGHT / 2u);
    g_pointer_buttons = 0u;
    g_pointer_saved = 0u;

    system_id = wm_window_create_locked("System", 28u, 70u,
                                        270u, 176u, COLOR_ACCENT);
    devices_id = wm_window_create_locked("Devices", 318u, 86u,
                                         294u, 190u, COLOR_OK);
    console_id = wm_window_create_locked("Console", 102u, 282u,
                                         440u, 142u, 0x00A78BFAu);

    (void)wm_window_set_line(system_id, 0u, "LAMP KERNEL 0.31", COLOR_TEXT);
    (void)wm_window_set_line(system_id, 1u, "640x480 XRGB8888", COLOR_MUTED);
    (void)wm_window_set_line(system_id, 2u,
        g_double_buffered ? "DOUBLE BUFFERED" : "SINGLE BUFFER FALLBACK",
        g_double_buffered ? COLOR_OK : COLOR_WARN);
    (void)wm_window_set_line(system_id, 3u, "4 MIB PCI VRAM", COLOR_MUTED);

    (void)wm_window_set_line(devices_id, 0u,
        gpu_active() ? "GPU       ONLINE" : "GPU       FALLBACK",
        gpu_active() ? COLOR_OK : COLOR_WARN);
    (void)wm_window_set_line(devices_id, 1u,
        pci_ether_bar0() != 0u ? "NETWORK   ONLINE" : "NETWORK   OFFLINE",
        pci_ether_bar0() != 0u ? COLOR_OK : COLOR_WARN);
    (void)wm_window_set_line(devices_id, 2u,
        audio_active() ? "AUDIO     ONLINE" : "AUDIO     OFFLINE",
        audio_active() ? COLOR_OK : COLOR_WARN);
    (void)wm_window_set_line(devices_id, 3u,
        g_hardware_cursor ? "MSI + CURSOR PLANE" : "MSI + DAMAGE FLUSH",
        COLOR_MUTED);

    (void)wm_window_set_line(console_id, 0u,
                             "Serial owns the shell.", COLOR_TEXT);
    (void)wm_window_set_line(console_id, 1u,
                             "PS/2 mouse owns the pointer.", COLOR_MUTED);
    (void)wm_window_set_line(console_id, 2u,
                             "Click a window to raise it.", COLOR_ACCENT);
    (void)wm_window_set_line(console_id, 3u,
                             "Click display to capture mouse.", COLOR_MUTED);
    (void)wm_window_set_line(console_id, 4u,
                             "Ctrl+Cmd/Alt+G releases pointer.", COLOR_MUTED);

    kio_lock();
    console_fb_set_text_output(0u);
    g_wm_active = 1u;
    wm_render_locked();
    if (g_hardware_cursor) {
        gpu_cursor_update((uint32_t)g_pointer_x, (uint32_t)g_pointer_y,
                          g_pointer_buttons, 1u);
    }
    kio_unlock();

    KLOGI(WM_TAG, "desktop active; serial keyboard, PS/2 pointer");
}

void wm_start_compositor(void) {
    if (!g_wm_active || g_compositor_started) {
        return;
    }
    if (sched_spawn("wm-compositor", wm_compositor_task, 0) < 0) {
        KLOGW(WM_TAG, "compositor task spawn failed");
        return;
    }
    g_compositor_started = 1u;
    KLOGI(WM_TAG, "compositor task started");
}

uint32_t wm_active(void) {
    return g_wm_active ? 1u : 0u;
}

uint32_t wm_window_create(const char *title,
                          uint32_t x, uint32_t y,
                          uint32_t width, uint32_t height,
                          uint32_t accent) {
    uint32_t id;
    kio_lock();
    id = wm_window_create_locked(title, x, y, width, height, accent);
    if (id != 0u && g_wm_active) {
        wm_render_locked();
    }
    kio_unlock();
    return id;
}

uint32_t wm_window_set_line(uint32_t id, uint32_t line,
                            const char *text, uint32_t color) {
    wm_window_t *window;
    if (line >= WM_MAX_LINES) {
        return 0u;
    }
    kio_lock();
    window = wm_find_window(id, (uint32_t *)0);
    if (!window) {
        kio_unlock();
        return 0u;
    }
    wm_copy_text(window->lines[line], WM_LINE_MAX, text);
    window->line_colors[line] = color;
    if (g_wm_active) {
        wm_render_locked();
    }
    kio_unlock();
    return 1u;
}

uint32_t wm_window_move(uint32_t id, uint32_t x, uint32_t y) {
    wm_window_t *window;
    kio_lock();
    window = wm_find_window(id, (uint32_t *)0);
    if (!window) {
        kio_unlock();
        return 0u;
    }
    if (x > FB_WIDTH - window->width - 8u) x = FB_WIDTH - window->width - 8u;
    if (y < 44u) y = 44u;
    if (y > FB_HEIGHT - window->height - 36u) {
        y = FB_HEIGHT - window->height - 36u;
    }
    window->x = x;
    window->y = y;
    if (g_wm_active) wm_render_locked();
    kio_unlock();
    return 1u;
}

uint32_t wm_window_raise(uint32_t id) {
    uint32_t index;
    kio_lock();
    if (!wm_find_window(id, &index)) {
        kio_unlock();
        return 0u;
    }
    wm_raise_index_locked(index);
    if (g_wm_active) wm_render_locked();
    kio_unlock();
    return 1u;
}

uint32_t wm_window_set_visible(uint32_t id, uint32_t visible) {
    wm_window_t *window;
    kio_lock();
    window = wm_find_window(id, (uint32_t *)0);
    if (!window) {
        kio_unlock();
        return 0u;
    }
    window->visible = visible ? 1u : 0u;
    if (g_wm_active) wm_render_locked();
    kio_unlock();
    return 1u;
}

uint32_t wm_window_count(void) {
    return g_window_count;
}

void wm_pointer_event(int32_t dx, int32_t dy, uint32_t buttons) {
    uint32_t old_x;
    uint32_t old_y;
    uint32_t hit;
    int32_t next_x;
    int32_t next_y;
    if (!g_wm_active) {
        return;
    }

    if (g_hardware_cursor && gpu_cursor_available()) {
        const uint32_t previous_buttons = g_pointer_buttons;
        next_x = g_pointer_x + dx;
        next_y = g_pointer_y + dy;
        if (next_x < 0) next_x = 0;
        if (next_y < 0) next_y = 0;
        if (next_x >= (int32_t)FB_WIDTH) next_x = (int32_t)FB_WIDTH - 1;
        if (next_y >= (int32_t)FB_HEIGHT) next_y = (int32_t)FB_HEIGHT - 1;
        g_pointer_x = next_x;
        g_pointer_y = next_y;
        g_pointer_buttons = buttons & 7u;

        /* The common path bypasses the compositor lock. The host serializes
         * the cursor command against a page flip, and each flip redraws the
         * active cursor plane over the new scanout. */
        gpu_cursor_update((uint32_t)next_x, (uint32_t)next_y,
                          g_pointer_buttons, 1u);

        if ((g_pointer_buttons & 1u) == 0u ||
            (previous_buttons & 1u) != 0u) {
            return;
        }

        /* Window policy is a compositor operation. Never take kio_lock from
         * the hard mouse IRQ: it may have interrupted the lock owner. */
        wm_queue_click_from_irq((uint32_t)next_x, (uint32_t)next_y);
        return;
    }

    kio_lock();
    g_hardware_cursor = 0u;
    old_x = (uint32_t)g_pointer_x;
    old_y = (uint32_t)g_pointer_y;
    wm_pointer_restore_locked();

    next_x = g_pointer_x + dx;
    next_y = g_pointer_y + dy;
    if (next_x < 0) next_x = 0;
    if (next_y < 0) next_y = 0;
    if (next_x >= (int32_t)FB_WIDTH) next_x = (int32_t)FB_WIDTH - 1;
    if (next_y >= (int32_t)FB_HEIGHT) next_y = (int32_t)FB_HEIGHT - 1;
    g_pointer_x = next_x;
    g_pointer_y = next_y;

    if ((buttons & 1u) != 0u && (g_pointer_buttons & 1u) == 0u) {
        hit = wm_hit_test_locked((uint32_t)g_pointer_x,
                                 (uint32_t)g_pointer_y);
        if (hit != 0u && g_z_order[g_window_count - 1u] != hit - 1u) {
            g_pointer_buttons = buttons & 7u;
            wm_raise_index_locked(hit - 1u);
            wm_request_redraw_locked();
            kio_unlock();
            return;
        }
    }
    g_pointer_buttons = buttons & 7u;
    wm_pointer_draw_locked();
    wm_pointer_present_locked(old_x, old_y);
    kio_unlock();
}
