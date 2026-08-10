#include "../include/kernel/platform.h"
#include "../include/kernel/mmu.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/smp.h"
#include "../include/kernel/spinlock.h"
#include "../include/kernel/vm_info.h"

#define SMP_MAX_CPUS 32u

#define SMP_STR1(x) #x
#define SMP_STR(x) SMP_STR1(x)

static volatile uint32_t g_online_cpus;
static volatile uint32_t g_target_cpus;
static volatile uint32_t g_cpu_online[SMP_MAX_CPUS];
static spinlock_t g_smp_lock;

void smp_ap_bootstrap(void);

uint32_t smp_cpu_id(void) {
    uint32_t id = 0u;
    __asm__ volatile("cpuid %0" : "=r"(id));
    if (id >= SMP_MAX_CPUS) {
        return 0u;
    }
    return id;
}

static inline void smp_start_ap_raw(uint32_t target_cpu, uint32_t entry_addr) {
    __asm__ volatile(
        "startap %0, %1, 0\n"
        :
        : "r"(target_cpu), "r"(entry_addr)
        : "memory"
    );
}

static void smp_mark_online(uint32_t cpu_id) {
    if (cpu_id >= SMP_MAX_CPUS) {
        return;
    }
    spinlock_lock(&g_smp_lock);
    if (g_cpu_online[cpu_id] == 0u) {
        g_cpu_online[cpu_id] = 1u;
        g_online_cpus++;
    }
    spinlock_unlock(&g_smp_lock);
}

static __attribute__((used)) void smp_ap_entry(void) {
    smp_init_ap();
    sched_run();
    for (;;) {
        __asm__ volatile("pause\n" ::: "memory");
    }
}

__asm__(
    ".text\n"
    ".globl smp_ap_bootstrap\n"
    "smp_ap_bootstrap:\n"
    /* STARTAP enters with the VM's firmware stack bases.  Those bases are
     * deliberately a VM implementation detail and may live above the
     * kernel's fixed 64 MiB identity map when the VM is given extra RAM.
     * Move every AP onto its kernel-owned reserved slot before the first
     * CALL, so enabling paging cannot strand a scheduler return address in
     * an unmapped firmware stack. */
    "  cpuid r0\n"
    "  movi r1, " SMP_STR(VM_STACK_POOL_BASE) "\n"
    "  movi r2, " SMP_STR(VM_STACK_SLOT_BYTES) "\n"
    "  mul r2, r0, r2\n"
    "  add r1, r1, r2\n"

    "  movi r2, " SMP_STR(IO_CPU_CTX_CALL_BASE) "\n"
    "  out r1, r2\n"
    "  movi r3, " SMP_STR(VM_CALL_STACK_BYTES) "\n"
    "  add r3, r1, r3\n"
    "  movi r2, " SMP_STR(IO_CPU_CTX_DATA_BASE) "\n"
    "  out r3, r2\n"
    "  movi r4, " SMP_STR(VM_DATA_STACK_BYTES) "\n"
    "  add r4, r3, r4\n"
    "  movi r2, " SMP_STR(IO_CPU_CTX_ISR_BASE) "\n"
    "  out r4, r2\n"

    "  movi r3, " SMP_STR(VM_CALL_STACK_ENTRIES) "\n"
    "  movi r2, " SMP_STR(IO_CPU_CTX_CSP) "\n"
    "  out r3, r2\n"
    "  movi r3, " SMP_STR(VM_DATA_STACK_ENTRIES) "\n"
    "  movi r2, " SMP_STR(IO_CPU_CTX_DSP) "\n"
    "  out r3, r2\n"
    "  movi r3, " SMP_STR(VM_ISR_STACK_ENTRIES) "\n"
    "  movi r2, " SMP_STR(IO_CPU_CTX_ISP) "\n"
    "  out r3, r2\n"

    "  movi r2, " SMP_STR(VM_STACK_SLOT_BYTES) "\n"
    "  add r30, r1, r2\n"
    "  mov r31, r30\n"
    "  call smp_ap_entry\n"
    "smp_ap_bootstrap_halt:\n"
    "  halt\n"
    "  jmp smp_ap_bootstrap_halt\n"
);

void smp_init_bsp(void) {
    boot_info_t info;
    uint32_t target = 1u;

    spinlock_init(&g_smp_lock);
    for (uint32_t i = 0u; i < SMP_MAX_CPUS; i++) {
        g_cpu_online[i] = 0u;
    }
    g_online_cpus = 0u;

    if (vm_info_load_boot(&info) &&
        (info.features & BOOTINFO_FEATURE_SMP) != 0u &&
        info.smp_cores > 1u) {
        target = info.smp_cores;
    }
    if (target > SMP_MAX_CPUS) {
        target = SMP_MAX_CPUS;
    }
    g_target_cpus = target;
    smp_mark_online(0u);
}

void smp_init_ap(void) {
    const uint32_t cpu_id = smp_cpu_id();
    if (cpu_id == 0u || cpu_id >= g_target_cpus) {
        return;
    }
    mmu_init_ap();
    smp_mark_online(cpu_id);
}

void smp_start_aps(void) {
    uint32_t target = g_target_cpus;
    uint32_t entry = (uint32_t)(uintptr_t)&smp_ap_bootstrap;

    if (target <= 1u) {
        return;
    }

    for (uint32_t cpu = 1u; cpu < target; cpu++) {
        smp_start_ap_raw(cpu, entry);
    }

    while (smp_online_cpus() < target) {
        __asm__ volatile("pause\n" ::: "memory");
    }
}

uint32_t smp_online_cpus(void) {
    uint32_t v;
    spinlock_lock(&g_smp_lock);
    v = g_online_cpus;
    spinlock_unlock(&g_smp_lock);
    return v;
}

uint32_t smp_target_cpus(void) {
    return g_target_cpus;
}
