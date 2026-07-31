#include "../include/kernel/blk.h"
#include "../include/kernel/audio.h"
#include "../include/kernel/console.h"
#include "../include/kernel/irq.h"
#include "../include/kernel/gpu.h"
#include "../include/kernel/graphics.h"
#include "../include/kernel/panic.h"
#include "../include/kernel/platform.h"
#include "net/net.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/syscall.h"
#include "../include/kernel/trap.h"
#include "../include/kernel/types.h"
#include "../include/kernel/wm.h"

#define STR1(x) #x
#define STR(x) STR1(x)
#define VM_ISR_STACK_ENTRY_SHIFT 11u

_Static_assert((1u << VM_ISR_STACK_ENTRY_SHIFT) == VM_ISR_STACK_ENTRIES,
               "IRQ frame indexing must match the per-CPU ISR stack size");

volatile uint32_t g_irq_stub_no[32];
volatile uint32_t g_irq_stub_r0[32];
volatile uint32_t g_irq_stub_r1[32];
volatile uint32_t g_irq_stub_r2[32];
volatile uint32_t g_irq_stub_r3[32];
volatile uint32_t g_irq_stub_r4[32];
volatile uint32_t g_irq_stub_r5[32];
volatile uint32_t g_irq_stub_r6[32];
volatile uint32_t g_irq_stub_abi_addr[32];
volatile uint32_t g_irq_stub_from_user[32];
volatile uint32_t g_irq_stub_saved_sp[32];
volatile uint32_t g_irq_stub_saved_fp[32];
volatile uint32_t g_irq_frame_from_user[32u * VM_ISR_STACK_ENTRIES];
volatile uint32_t g_irq_frame_saved_sp[32u * VM_ISR_STACK_ENTRIES];
volatile uint32_t g_irq_frame_saved_fp[32u * VM_ISR_STACK_ENTRIES];
volatile uint32_t g_irq_frame_irq_masked[32u * VM_ISR_STACK_ENTRIES];
volatile uint32_t g_irq_saved_user_csp[32];
volatile uint32_t g_irq_saved_user_dsp[32];
volatile uint32_t g_irq_saved_user_call_base[32];
volatile uint32_t g_irq_saved_user_data_base[32];
static volatile trap_frame_t g_last_trap_frame;
static volatile uint32_t g_irq_counts[KERNEL_IVT_SIZE];
static volatile uint32_t g_fault_div0;
static volatile uint32_t g_ps2_kbd_shift_l;
static volatile uint32_t g_ps2_kbd_shift_r;
static volatile uint32_t g_ps2_kbd_ctrl_l;
static volatile uint32_t g_ps2_kbd_ctrl_r;
static volatile uint32_t g_ps2_kbd_caps_lock;
static volatile uint32_t g_ps2_kbd_ext;
static uint8_t g_ps2_mouse_packet[3];
static uint32_t g_ps2_mouse_packet_index;

static const uint8_t g_ps2_kbd_ascii_plain[128] = {
    [0x01] = 0x1Bu,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x0E] = 0x08u, [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/', [0x39] = ' '
};

static const uint8_t g_ps2_kbd_ascii_shift[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M', [0x33] = '<',
    [0x34] = '>', [0x35] = '?', [0x39] = ' '
};

static inline uint32_t io_in32(uint32_t addr) {
    uint32_t v;
    __asm__ volatile ("in %0, %1" : "=r"(v) : "r"(addr));
    return v;
}

static inline void io_out32(uint32_t addr, uint32_t value) {
    __asm__ volatile ("out %0, %1" :: "r"(value), "r"(addr));
}

static int ps2_read_data_wait(uint8_t *out, uint32_t want_aux, uint32_t check_aux) {
    for (uint32_t i = 0; i < 1024u; i++) {
        const uint32_t status = io_in32(IO_PS2_STATUS);
        if ((status & PS2_STATUS_OUT_FULL) == 0u) {
            continue;
        }
        if (check_aux != 0u && (((status & PS2_STATUS_AUX_DATA) != 0u) ? 1u : 0u) != want_aux) {
            return 0;
        }
        *out = (uint8_t)(io_in32(IO_PS2_DATA) & 0xFFu);
        return 1;
    }
    return 0;
}

static void ps2_flush_output(void) {
    for (uint32_t i = 0; i < 64u; i++) {
        if ((io_in32(IO_PS2_STATUS) & PS2_STATUS_OUT_FULL) == 0u) {
            return;
        }
        (void)io_in32(IO_PS2_DATA);
    }
}

static void ps2_write_controller(uint8_t command) {
    io_out32(IO_PS2_COMMAND, command);
}

static void ps2_write_data(uint8_t value) {
    io_out32(IO_PS2_DATA, value);
}

static void ps2_write_mouse(uint8_t value) {
    ps2_write_controller(0xD4u);
    ps2_write_data(value);
}

static void ps2_read_ack(uint32_t aux) {
    uint8_t value = 0u;
    (void)ps2_read_data_wait(&value, aux, 1u);
}

static inline uint32_t irq_stub_cpu_index(void) {
    uint32_t cpu = 0u;
    __asm__ volatile("cpuid %0" : "=r"(cpu));
    if (cpu >= 32u) {
        cpu = 0u;
    }
    return cpu;
}

void irq_serial_drain_rx(void) {
    while ((io_in32(IO_SERIAL_STATUS) & SERIAL_STATUS_RX_READY) != 0u) {
        uint32_t v = io_in32(IO_SERIAL_RX);
        klog_begin(KLOG_LEVEL_DEBUG, "serial");
        klog_puts("rx=");
        klog_hex32(v & 0xFFu);
        klog_end();
        console_rx_feed((uint8_t)(v & 0xFFu));
    }
}

static void ps2_keyboard_handle_scancode(uint8_t scancode) {
    uint32_t release;
    uint8_t scan;
    uint32_t shift;
    uint32_t ctrl;
    uint8_t out = 0u;

    /* The graphical VM Display reserves PS/2 input for a future GUI event
     * queue. Interactive text input remains on the serial console. */
    if (graphics_active()) {
        return;
    }

    if (scancode == 0xE0u) {
        g_ps2_kbd_ext = 1u;
        return;
    }

    release = (scancode & 0x80u) ? 1u : 0u;
    scan = (uint8_t)(scancode & 0x7Fu);

    if (g_ps2_kbd_ext != 0u) {
        g_ps2_kbd_ext = 0u;
        if (scan == 0x1Du) {
            g_ps2_kbd_ctrl_r = release ? 0u : 1u;
        }
        return;
    }

    if (scan == 0x2Au) {
        g_ps2_kbd_shift_l = release ? 0u : 1u;
        return;
    }
    if (scan == 0x36u) {
        g_ps2_kbd_shift_r = release ? 0u : 1u;
        return;
    }
    if (scan == 0x1Du) {
        g_ps2_kbd_ctrl_l = release ? 0u : 1u;
        return;
    }
    if (scan == 0x3Au) {
        if (!release) {
            g_ps2_kbd_caps_lock ^= 1u;
        }
        return;
    }
    if (release) {
        return;
    }

    shift = (g_ps2_kbd_shift_l | g_ps2_kbd_shift_r) ? 1u : 0u;
    ctrl = (g_ps2_kbd_ctrl_l | g_ps2_kbd_ctrl_r) ? 1u : 0u;

    if (ctrl != 0u) {
        const uint8_t base = g_ps2_kbd_ascii_plain[scan];
        if (base >= 'a' && base <= 'z') {
            out = (uint8_t)(base - 'a' + 1u);
        } else if (base >= 'A' && base <= 'Z') {
            out = (uint8_t)(base - 'A' + 1u);
        }
    } else {
        out = shift ? g_ps2_kbd_ascii_shift[scan] : g_ps2_kbd_ascii_plain[scan];
        if (g_ps2_kbd_caps_lock != 0u && out >= 'a' && out <= 'z') {
            out = (uint8_t)(out - 'a' + 'A');
        } else if (g_ps2_kbd_caps_lock != 0u && out >= 'A' && out <= 'Z' && shift != 0u) {
            out = (uint8_t)(out - 'A' + 'a');
        }
    }

    if (out != 0u) {
        console_rx_feed(out);
    }
}

static void ps2_keyboard_drain_rx(void) {
    while ((io_in32(IO_PS2_STATUS) & PS2_STATUS_OUT_FULL) != 0u) {
        if ((io_in32(IO_PS2_STATUS) & PS2_STATUS_AUX_DATA) != 0u) {
            return;
        }
        const uint32_t v = io_in32(IO_PS2_DATA);
        ps2_keyboard_handle_scancode((uint8_t)(v & 0xFFu));
    }
}

static void ps2_mouse_drain_rx(void) {
    int32_t accumulated_dx = 0;
    int32_t accumulated_dy = 0;
    uint32_t accumulated_buttons = 0u;
    uint32_t have_packet = 0u;
    while ((io_in32(IO_PS2_STATUS) & PS2_STATUS_OUT_FULL) != 0u) {
        if ((io_in32(IO_PS2_STATUS) & PS2_STATUS_AUX_DATA) == 0u) {
            break;
        }
        const uint8_t value = (uint8_t)(io_in32(IO_PS2_DATA) & 0xFFu);
        if (g_ps2_mouse_packet_index == 0u && (value & 0x08u) == 0u) {
            continue;
        }
        g_ps2_mouse_packet[g_ps2_mouse_packet_index++] = value;
        if (g_ps2_mouse_packet_index == 3u) {
            int32_t dx;
            int32_t dy;
            uint32_t buttons;
            const uint8_t flags = g_ps2_mouse_packet[0];
            const uint8_t raw_x = g_ps2_mouse_packet[1];
            const uint8_t raw_y = g_ps2_mouse_packet[2];
            g_ps2_mouse_packet_index = 0u;
            dx = (raw_x & 0x80u) != 0u ?
                (int32_t)((uint32_t)raw_x | 0xFFFFFF00u) : (int32_t)raw_x;
            dy = (raw_y & 0x80u) != 0u ?
                (int32_t)((uint32_t)raw_y | 0xFFFFFF00u) : (int32_t)raw_y;
            if ((flags & 0x40u) != 0u) dx = 0;
            if ((flags & 0x80u) != 0u) dy = 0;
            buttons = flags & 0x07u;
            if (have_packet && buttons != accumulated_buttons) {
                wm_pointer_event(accumulated_dx, accumulated_dy,
                                 accumulated_buttons);
                accumulated_dx = 0;
                accumulated_dy = 0;
                have_packet = 0u;
            }
            accumulated_dx += dx;
            /* PS/2 Y grows upward; framebuffer Y grows downward. */
            accumulated_dy -= dy;
            accumulated_buttons = buttons;
            have_packet = 1u;
        }
    }
    if (have_packet) {
        wm_pointer_event(accumulated_dx, accumulated_dy,
                         accumulated_buttons);
    }
}

void irq_common_entry(uint32_t irq_no) {
    g_last_trap_frame.irq_no = irq_no;
    g_last_trap_frame.dispatch_count++;
    g_last_trap_frame.tick_snapshot = sched_ticks();
    if (irq_no == IRQ_SYSCALL) {
        irq_syscall(irq_no);
        irq_eoi(irq_no);
        return;
    }
    switch (irq_no) {
        case IRQ_SERIAL:
            irq_serial(irq_no);
            break;
        case IRQ_DISK_COMPLETE:
            irq_disk_complete(irq_no);
            break;
        case IRQ_TIMER:
            irq_timer(irq_no);
            break;
        case IRQ_KEYBOARD:
            irq_keyboard(irq_no);
            break;
        case IRQ_MOUSE:
            irq_mouse(irq_no);
            break;
        case IRQ_ETHER:
            irq_ether(irq_no);
            break;
        case IRQ_GPU:
            irq_gpu(irq_no);
            break;
        case IRQ_AUDIO:
            irq_audio(irq_no);
            break;
        case IRQ_DIVIDE_BY_ZERO:
            irq_divide_by_zero(irq_no);
            break;
        default:
            trap_dispatch(irq_no);
            break;
    }
    irq_eoi(irq_no);
}

void irq_common_entry_from_stub(void) {
    const uint32_t cpu = irq_stub_cpu_index();
    if (g_irq_stub_from_user[cpu] != 0u) {
        sched_save_current_user_irq_ctx();
    }
    irq_common_entry(g_irq_stub_no[cpu]);
    if (g_irq_stub_from_user[cpu] != 0u) {
        sched_signal_deliver_pending();
    }
}

void irq_input_init(void) {
    g_ps2_mouse_packet_index = 0u;
    io_out32(IO_SERIAL_STATUS, SERIAL_CTRL_RX_INT_ENABLE);
    ps2_write_controller(0xADu);
    ps2_write_controller(0xA7u);
    ps2_flush_output();

    ps2_write_controller(0x20u);
    uint8_t config = 0u;
    if (ps2_read_data_wait(&config, 0u, 0u)) {
        config |= 0x03u;
        config &= (uint8_t)~0x30u;
        ps2_write_controller(0x60u);
        ps2_write_data(config);
    }

    ps2_write_controller(0xAEu);
    ps2_write_controller(0xA8u);

    ps2_write_data(0xF4u);
    ps2_read_ack(0u);

    ps2_write_mouse(0xF6u);
    ps2_read_ack(1u);
    ps2_write_mouse(0xF4u);
    ps2_read_ack(1u);
}

uint32_t irq_input_dropped(void) {
    return console_rx_dropped();
}

const trap_frame_t *irq_last_trap_frame(void) {
    return (const trap_frame_t *)&g_last_trap_frame;
}

uint32_t irq_saved_user_csp(void) {
    uint32_t cpu = 0u;
    __asm__ volatile("cpuid %0" : "=r"(cpu));
    if (cpu >= 32u) {
        cpu = 0u;
    }
    return g_irq_saved_user_csp[cpu];
}

uint32_t irq_saved_user_dsp(void) {
    uint32_t cpu = 0u;
    __asm__ volatile("cpuid %0" : "=r"(cpu));
    if (cpu >= 32u) {
        cpu = 0u;
    }
    return g_irq_saved_user_dsp[cpu];
}

uint32_t irq_saved_user_call_base(void) {
    uint32_t cpu = 0u;
    __asm__ volatile("cpuid %0" : "=r"(cpu));
    if (cpu >= 32u) {
        cpu = 0u;
    }
    return g_irq_saved_user_call_base[cpu];
}

uint32_t irq_saved_user_data_base(void) {
    uint32_t cpu = 0u;
    __asm__ volatile("cpuid %0" : "=r"(cpu));
    if (cpu >= 32u) {
        cpu = 0u;
    }
    return g_irq_saved_user_data_base[cpu];
}

void irq_saved_user_ctx_set(uint32_t csp, uint32_t dsp, uint32_t call_base, uint32_t data_base) {
    uint32_t cpu = 0u;
    __asm__ volatile("cpuid %0" : "=r"(cpu));
    if (cpu >= 32u) {
        cpu = 0u;
    }
    g_irq_saved_user_csp[cpu] = csp;
    g_irq_saved_user_dsp[cpu] = dsp;
    g_irq_saved_user_call_base[cpu] = call_base;
    g_irq_saved_user_data_base[cpu] = data_base;
}

static inline uint32_t irq_isr_slot_read32(uint32_t isr_base, uint32_t index) {
    return *(volatile uint32_t *)(uintptr_t)(isr_base + index * 8u);
}

static inline void irq_isr_slot_write32(uint32_t isr_base, uint32_t index, uint32_t value) {
    volatile uint32_t *slot = (volatile uint32_t *)(uintptr_t)(isr_base + index * 8u);
    slot[0] = value;
    slot[1] = 0u;
}

int irq_user_context_get(irq_user_context_t *ctx) {
    uint32_t cpu;
    uint32_t isp;
    uint32_t isr_base;
    uint32_t frame_index;
    if (!ctx) {
        return -1;
    }
    cpu = irq_stub_cpu_index();
    if (g_irq_stub_from_user[cpu] == 0u) {
        return -1;
    }
    isp = io_in32(IO_CPU_CTX_ISP);
    isr_base = io_in32(IO_CPU_CTX_ISR_BASE);
    if (isp > VM_ISR_STACK_ENTRIES - 34u || isr_base == 0u) {
        return -1;
    }
    for (uint32_t reg = 0u; reg < 32u; reg++) {
        ctx->regs[reg] = irq_isr_slot_read32(isr_base, isp + (31u - reg));
    }
    ctx->flags = irq_isr_slot_read32(isr_base, isp + 32u);
    ctx->ip = irq_isr_slot_read32(isr_base, isp + 33u);
    ctx->csp = g_irq_saved_user_csp[cpu];
    ctx->dsp = g_irq_saved_user_dsp[cpu];
    ctx->call_base = g_irq_saved_user_call_base[cpu];
    ctx->data_base = g_irq_saved_user_data_base[cpu];
    frame_index = cpu * VM_ISR_STACK_ENTRIES + isp;
    ctx->irq_masked = g_irq_frame_irq_masked[frame_index];
    return 0;
}

int irq_user_context_set(const irq_user_context_t *ctx) {
    uint32_t cpu;
    uint32_t isp;
    uint32_t isr_base;
    uint32_t frame_index;
    if (!ctx || ctx->csp > VM_CALL_STACK_ENTRIES || ctx->dsp > VM_DATA_STACK_ENTRIES) {
        return -1;
    }
    cpu = irq_stub_cpu_index();
    if (g_irq_stub_from_user[cpu] == 0u) {
        return -1;
    }
    isp = io_in32(IO_CPU_CTX_ISP);
    isr_base = io_in32(IO_CPU_CTX_ISR_BASE);
    if (isp > VM_ISR_STACK_ENTRIES - 34u || isr_base == 0u) {
        return -1;
    }
    for (uint32_t reg = 0u; reg < 32u; reg++) {
        irq_isr_slot_write32(isr_base, isp + (31u - reg), ctx->regs[reg]);
    }
    irq_isr_slot_write32(isr_base, isp + 32u, ctx->flags);
    irq_isr_slot_write32(isr_base, isp + 33u, ctx->ip);
    g_irq_saved_user_csp[cpu] = ctx->csp;
    g_irq_saved_user_dsp[cpu] = ctx->dsp;
    g_irq_saved_user_call_base[cpu] = ctx->call_base;
    g_irq_saved_user_data_base[cpu] = ctx->data_base;
    frame_index = cpu * VM_ISR_STACK_ENTRIES + isp;
    g_irq_frame_irq_masked[frame_index] = ctx->irq_masked ? 1u : 0u;
    return 0;
}

void irq_default(uint32_t irq_no) {
    if (irq_no < KERNEL_IVT_SIZE) {
        g_irq_counts[irq_no]++;
    }
}

void irq_divide_by_zero(uint32_t irq_no) {
    (void)irq_no;
    g_fault_div0 = 1u;
    kpanic("divide by zero");
}

void irq_disk_complete(uint32_t irq_no) {
    if (irq_no < KERNEL_IVT_SIZE) {
        g_irq_counts[irq_no]++;
    }
    blk_irq_complete();
}

void irq_serial(uint32_t irq_no) {
    if (irq_no < KERNEL_IVT_SIZE) {
        g_irq_counts[irq_no]++;
    }
    irq_serial_drain_rx();
}

void irq_keyboard(uint32_t irq_no) {
    if (irq_no < KERNEL_IVT_SIZE) {
        g_irq_counts[irq_no]++;
    }
    ps2_keyboard_drain_rx();
}

void irq_mouse(uint32_t irq_no) {
    if (irq_no < KERNEL_IVT_SIZE) {
        g_irq_counts[irq_no]++;
    }
    ps2_mouse_drain_rx();
}

void irq_ether(uint32_t irq_no) {
    if (irq_no < KERNEL_IVT_SIZE) g_irq_counts[irq_no]++;
    ether_irq_handler();
}

void irq_gpu(uint32_t irq_no) {
    if (irq_no < KERNEL_IVT_SIZE) g_irq_counts[irq_no]++;
    gpu_irq_handler();
}

void irq_audio(uint32_t irq_no) {
    if (irq_no < KERNEL_IVT_SIZE) g_irq_counts[irq_no]++;
    audio_irq_handler();
}

void irq_timer(uint32_t irq_no) {
    if (irq_no < KERNEL_IVT_SIZE) {
        g_irq_counts[irq_no]++;
    }
    schedule_tick();
}

void irq_syscall(uint32_t irq_no) {
    syscall_regs_t regs;
    if (irq_no < KERNEL_IVT_SIZE) {
        g_irq_counts[irq_no]++;
    }
    /*
     * Keep IRQ syscall entry on a pointer-based call path.
     * This avoids relying on wide C argument passing in the current backend.
     */
    const uint32_t cpu = irq_stub_cpu_index();
    regs.nr = g_irq_stub_r0[cpu];
    regs.arg0 = g_irq_stub_r1[cpu];
    regs.arg1 = g_irq_stub_r2[cpu];
    regs.arg2 = g_irq_stub_r3[cpu];
    regs.arg3 = g_irq_stub_r4[cpu];
    regs.arg4 = g_irq_stub_r5[cpu];
    regs.arg5 = g_irq_stub_r6[cpu];
    regs.abi_addr = g_irq_stub_abi_addr[cpu];
    (void)syscall_dispatch(&regs);
}

__asm__(
    ".text\n"
    ".globl irq_stub_entry\n"
    "irq_stub_entry:\n"
    "  mov r16, r8\n"
    "  mov r8, r0\n"
    "  mov r9, r1\n"
    "  mov r10, r2\n"
    "  mov r11, r3\n"
    "  mov r12, r4\n"
    "  mov r13, r5\n"
    "  mov r14, r6\n"
    "  mov r2, r31\n"
    "  mov r17, r2\n"
    "  cpuid r15\n"
    "  mov r7, r15\n"
    "  add r7, r7, r7\n"
    "  add r7, r7, r7\n"
    "  movi r1, " STR(IO_CPU_CTX_ISP) "\n"
    "  in r18, r1\n"
    "  mov r19, r15\n"
    "  shli r19, r19, " STR(VM_ISR_STACK_ENTRY_SHIFT) "\n"
    "  add r19, r19, r18\n"
    "  shli r19, r19, 2\n"
    "  movi r0, " STR(IO_CPU_CTX_ISR_BASE) "\n"
    "  in r3, r0\n"
    "  movi r0, " STR((VM_ISR_STACK_ENTRIES - 1u) * 8u) "\n"
    "  add r3, r3, r0\n"
    "  load32 r0, r3, 0\n"
    "  shri r0, r0, 24\n"
    "  subi r0, r0, 2\n"
    "  movi r1, 0\n"
    "  sub r1, r1, r0\n"
    "  or r0, r0, r1\n"
    "  shri r0, r0, 31\n"
    "  xori r0, r0, 1\n"
    "  subi r0, r0, 0\n"
    "  movi r1, g_irq_stub_from_user\n"
    "  add r1, r1, r7\n"
    "  store32 r0, r1, 0\n"
    "  rjz r0, .Lirq_skip_user_ctx_save\n"
    "  movi r0, g_irq_saved_user_csp\n"
    "  add r0, r0, r7\n"
    "  movi r1, " STR(IO_CPU_CTX_CSP) "\n"
    "  in r3, r1\n"
    "  store32 r3, r0, 0\n"
    "  movi r0, g_irq_saved_user_dsp\n"
    "  add r0, r0, r7\n"
    "  movi r1, " STR(IO_CPU_CTX_DSP) "\n"
    "  in r3, r1\n"
    "  store32 r3, r0, 0\n"
    "  movi r0, g_irq_saved_user_call_base\n"
    "  add r0, r0, r7\n"
    "  movi r1, " STR(IO_CPU_CTX_CALL_BASE) "\n"
    "  in r3, r1\n"
    "  store32 r3, r0, 0\n"
    "  movi r0, g_irq_saved_user_data_base\n"
    "  add r0, r0, r7\n"
    "  movi r1, " STR(IO_CPU_CTX_DATA_BASE) "\n"
    "  in r3, r1\n"
    "  store32 r3, r0, 0\n"
    "  movi r0, g_irq_stub_from_user\n"
    "  add r0, r0, r7\n"
    "  load32 r0, r0, 0\n"
    ".Lirq_skip_user_ctx_save:\n"
    "  rjz r0, .Lirq_keep_current_stack\n"
    "  movi r0, " STR(VM_CALL_STACK_ENTRIES) "\n"
    "  movi r1, " STR(IO_CPU_CTX_CSP) "\n"
    "  out r0, r1\n"
    "  movi r0, " STR(VM_DATA_STACK_ENTRIES) "\n"
    "  movi r1, " STR(IO_CPU_CTX_DSP) "\n"
    "  out r0, r1\n"
    "  shli r15, r15, " STR(KERNEL_IRQ_STACK_SHIFT) "\n"
    "  movi r30, " STR(KERNEL_IRQ_STACK_TOP) "\n"
    "  sub r30, r30, r15\n"
    "  movi r0, " STR(KERNEL_IRQ_STACK_BYTES) "\n"
    "  sub r0, r30, r0\n"
    "  movi r1, " STR(IO_CPU_CTX_CALL_BASE) "\n"
    "  out r0, r1\n"
    "  movi r2, " STR(VM_CALL_STACK_BYTES) "\n"
    "  add r0, r0, r2\n"
    "  movi r1, " STR(IO_CPU_CTX_DATA_BASE) "\n"
    "  out r0, r1\n"
    "  mov r31, r30\n"
    ".Lirq_keep_current_stack:\n"
    "  mov r31, r30\n"
    "  subi r30, r30, 8\n"
    "  store32 r0, r30, 0\n"
    "  mov r20, r0\n"
    "  movi r0, g_irq_frame_from_user\n"
    "  add r0, r0, r19\n"
    "  store32 r20, r0, 0\n"
    "  movi r1, " STR(IO_CPU_CTX_IRQ_MASK) "\n"
    "  in r0, r1\n"
    "  movi r1, g_irq_frame_irq_masked\n"
    "  add r1, r1, r19\n"
    "  store32 r0, r1, 0\n"
    "  movi r0, 1\n"
    "  movi r1, " STR(IO_CPU_CTX_IRQ_MASK) "\n"
    "  out r0, r1\n"
    "  movi r0, g_irq_frame_saved_sp\n"
    "  add r0, r0, r19\n"
    "  store32 r30, r0, 0\n"
    "  movi r0, g_irq_frame_saved_fp\n"
    "  add r0, r0, r19\n"
    "  store32 r31, r0, 0\n"
    "  movi r0, g_irq_stub_saved_sp\n"
    "  add r0, r0, r7\n"
    "  store32 r30, r0, 0\n"
    "  movi r0, g_irq_stub_saved_fp\n"
    "  add r0, r0, r7\n"
    "  store32 r31, r0, 0\n"
    "  movi r0, g_irq_stub_no\n"
    "  add r0, r0, r7\n"
    "  store32 r17, r0, 0\n"
    "  movi r0, g_irq_stub_r0\n"
    "  add r0, r0, r7\n"
    "  store32 r8, r0, 0\n"
    "  movi r0, g_irq_stub_r1\n"
    "  add r0, r0, r7\n"
    "  store32 r9, r0, 0\n"
    "  movi r0, g_irq_stub_r2\n"
    "  add r0, r0, r7\n"
    "  store32 r10, r0, 0\n"
    "  movi r0, g_irq_stub_r3\n"
    "  add r0, r0, r7\n"
    "  store32 r11, r0, 0\n"
    "  movi r0, g_irq_stub_r4\n"
    "  add r0, r0, r7\n"
    "  store32 r12, r0, 0\n"
    "  movi r0, g_irq_stub_r5\n"
    "  add r0, r0, r7\n"
    "  store32 r13, r0, 0\n"
    "  movi r0, g_irq_stub_r6\n"
    "  add r0, r0, r7\n"
    "  store32 r14, r0, 0\n"
    "  movi r0, g_irq_stub_abi_addr\n"
    "  add r0, r0, r7\n"
    "  store32 r16, r0, 0\n"
    "  call irq_common_entry_from_stub\n"
    "  cpuid r7\n"
    "  movi r1, " STR(IO_CPU_CTX_ISP) "\n"
    "  in r18, r1\n"
    "  mov r19, r7\n"
    "  shli r19, r19, " STR(VM_ISR_STACK_ENTRY_SHIFT) "\n"
    "  add r19, r19, r18\n"
    "  shli r19, r19, 2\n"
    "  add r7, r7, r7\n"
    "  add r7, r7, r7\n"
    "  movi r0, g_irq_frame_saved_sp\n"
    "  add r0, r0, r19\n"
    "  load32 r30, r0, 0\n"
    "  movi r0, g_irq_frame_saved_fp\n"
    "  add r0, r0, r19\n"
    "  load32 r31, r0, 0\n"
    "  movi r0, g_irq_frame_from_user\n"
    "  add r0, r0, r19\n"
    "  load32 r0, r0, 0\n"
    "  rjz r0, .Lirq_skip_user_ctx_restore\n"
    "  movi r0, g_irq_saved_user_call_base\n"
    "  add r0, r0, r7\n"
    "  load32 r3, r0, 0\n"
    "  movi r1, " STR(IO_CPU_CTX_CALL_BASE) "\n"
    "  out r3, r1\n"
    "  movi r0, g_irq_saved_user_data_base\n"
    "  add r0, r0, r7\n"
    "  load32 r3, r0, 0\n"
    "  movi r1, " STR(IO_CPU_CTX_DATA_BASE) "\n"
    "  out r3, r1\n"
    "  movi r0, g_irq_saved_user_csp\n"
    "  add r0, r0, r7\n"
    "  load32 r3, r0, 0\n"
    "  movi r1, " STR(IO_CPU_CTX_CSP) "\n"
    "  out r3, r1\n"
    "  movi r0, g_irq_saved_user_dsp\n"
    "  add r0, r0, r7\n"
    "  load32 r3, r0, 0\n"
    "  movi r1, " STR(IO_CPU_CTX_DSP) "\n"
    "  out r3, r1\n"
    ".Lirq_skip_user_ctx_restore:\n"
    /* IRQ masking is CPU state, not user-stack state. It is saved for every
     * interrupt at entry and therefore must be restored for both kernel- and
     * user-originated interrupts. Keeping this inside the user-only branch
     * left kernel execution permanently masked after its first IRQ. */
    "  movi r0, g_irq_frame_irq_masked\n"
    "  add r0, r0, r19\n"
    "  load32 r1, r0, 0\n"
    "  movi r0, " STR(IO_CPU_CTX_IRQ_MASK) "\n"
    "  out r1, r0\n"
    "  iret\n"
);
