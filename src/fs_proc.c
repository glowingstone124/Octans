#include "../include/kernel/fs_proc.h"

#include "../include/kernel/platform.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/syscall.h"
#include "../include/kernel/vm_info.h"

enum {
    PROC_INO_ROOT = 1u,
    PROC_INO_CPUINFO = 2u,
    PROC_INO_MEMINFO = 3u,
    PROC_INO_UPTIME = 4u,
    PROC_INO_STAT = 5u,
    PROC_INO_VERSION = 6u,
    PROC_INO_LOADAVG = 7u,
    PROC_INO_LAMPVM = 8u,
    PROC_INO_TASK_DIR = 0x40000000u,
    PROC_INO_TASK_STAT = 0x80000000u,
    PROC_INO_TASK_PID_MASK = 0x3FFFFFFFu,
    PROC_CONTENT_CAP = 12288u
};

typedef struct proc_entry {
    uint32_t ino;
    const char *name;
    uint32_t type;
} proc_entry_t;

typedef struct proc_buffer {
    char *data;
    uint32_t capacity;
    uint32_t length;
} proc_buffer_t;

static const proc_entry_t g_proc_entries[] = {
    { PROC_INO_ROOT, ".", SYS_DT_DIR },
    { PROC_INO_ROOT, "..", SYS_DT_DIR },
    { PROC_INO_CPUINFO, "cpuinfo", SYS_DT_REG },
    { PROC_INO_MEMINFO, "meminfo", SYS_DT_REG },
    { PROC_INO_UPTIME, "uptime", SYS_DT_REG },
    { PROC_INO_STAT, "stat", SYS_DT_REG },
    { PROC_INO_VERSION, "version", SYS_DT_REG },
    { PROC_INO_LOADAVG, "loadavg", SYS_DT_REG },
    { PROC_INO_LAMPVM, "lampvm", SYS_DT_REG }
};

static uint32_t proc_is_task_dir(uint32_t ino) {
    return (ino & ~PROC_INO_TASK_PID_MASK) == PROC_INO_TASK_DIR;
}

static uint32_t proc_is_task_stat(uint32_t ino) {
    return (ino & ~PROC_INO_TASK_PID_MASK) == PROC_INO_TASK_STAT;
}

static uint32_t proc_task_pid(uint32_t ino) {
    return ino & PROC_INO_TASK_PID_MASK;
}

static uint32_t proc_directory_size(uint32_t ino) {
    if (ino == PROC_INO_ROOT) {
        return (uint32_t)(sizeof(g_proc_entries) / sizeof(g_proc_entries[0])) +
               SCHED_MAX_TASKS;
    }
    if (proc_is_task_dir(ino)) {
        return 3u; /* ., .., stat */
    }
    return 0u;
}

static inline uint32_t proc_read32(uint32_t addr) {
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static uint64_t proc_read64(uint32_t low_reg, uint32_t high_reg) {
    uint32_t low = proc_read32(SYSINFO_MMIO_BASE + low_reg);
    uint32_t high = proc_read32(SYSINFO_MMIO_BASE + high_reg);
    return ((uint64_t)high << 32) | low;
}

static uint32_t proc_str_eq(const char *a, const char *b) {
    uint32_t i = 0u;
    if (!a || !b) return 0u;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) return 0u;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0' ? 1u : 0u;
}

static void proc_buf_putc(proc_buffer_t *out, char c) {
    if (out->length + 1u < out->capacity) {
        out->data[out->length++] = c;
        out->data[out->length] = '\0';
    }
}

static void proc_buf_puts(proc_buffer_t *out, const char *text) {
    uint32_t i = 0u;
    if (!text) return;
    while (text[i] != '\0') {
        proc_buf_putc(out, text[i++]);
    }
}

static void proc_buf_u64(proc_buffer_t *out, uint64_t value) {
    char digits[24];
    uint32_t count = 0u;
    if (value == 0u) {
        proc_buf_putc(out, '0');
        return;
    }
    while (value != 0u && count < (uint32_t)sizeof(digits)) {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    }
    while (count != 0u) {
        proc_buf_putc(out, digits[--count]);
    }
}

static void proc_buf_i32(proc_buffer_t *out, int32_t value) {
    if (value < 0) {
        proc_buf_putc(out, '-');
        proc_buf_u64(out, (uint32_t)(-(value + 1)) + 1u);
        return;
    }
    proc_buf_u64(out, (uint32_t)value);
}

static char proc_task_state(uint32_t state) {
    switch (state) {
        case SCHED_TASK_RUNNING:
        case SCHED_TASK_RUNNABLE:
            return 'R';
        case SCHED_TASK_SLEEPING:
            return 'S';
        case SCHED_TASK_BLOCKED:
            return 'D';
        case SCHED_TASK_ZOMBIE:
            return 'Z';
        default:
            return 'S';
    }
}

static uint32_t proc_task_stat(uint32_t pid, char *dst, uint32_t capacity) {
    sched_proc_task_info_t task;
    proc_buffer_t out;
    uint32_t page_count;
    uint32_t virtual_size;

    if (!dst || capacity == 0u || sched_proc_task_by_pid(pid, &task) != 0) {
        return 0u;
    }
    out.data = dst;
    out.capacity = capacity;
    out.length = 0u;
    out.data[0] = '\0';
    page_count = (task.stack_bytes + 4095u) / 4096u;
    virtual_size = task.kind == SCHED_TASK_KIND_USER ?
        USER_REGION_SIZE : task.stack_bytes;

    /* Linux-compatible field order through rss. */
    proc_buf_u64(&out, task.pid);
    proc_buf_puts(&out, " (");
    proc_buf_puts(&out, task.name);
    proc_buf_puts(&out, ") ");
    proc_buf_putc(&out, proc_task_state(task.state));
    proc_buf_putc(&out, ' ');
    proc_buf_i32(&out, task.ppid);
    proc_buf_putc(&out, ' ');
    proc_buf_u64(&out, task.pid);     /* process group */
    proc_buf_putc(&out, ' ');
    proc_buf_u64(&out, task.pid);     /* session */
    proc_buf_puts(&out, " 0 0 0 0 0 0 0 ");
    proc_buf_u64(&out, task.run_ticks); /* utime */
    proc_buf_puts(&out, " 0 0 0 20 0 1 0 0 ");
    proc_buf_putc(&out, ' ');
    proc_buf_u64(&out, virtual_size);
    proc_buf_putc(&out, ' ');
    proc_buf_u64(&out, page_count);
    proc_buf_putc(&out, '\n');
    return out.length;
}

static int proc_parse_task_path(const char *path,
                                uint32_t *pid_out,
                                const char **tail_out) {
    const char *p;
    uint32_t pid = 0u;

    if (!path || path[0] != '/' || path[1] != 'p' || path[2] != 'r' ||
        path[3] != 'o' || path[4] != 'c' || path[5] != '/') {
        return 0;
    }
    p = path + 6u;
    if (*p < '0' || *p > '9') {
        return 0;
    }
    while (*p >= '0' && *p <= '9') {
        const uint32_t digit = (uint32_t)(*p - '0');
        if (pid > (PROC_INO_TASK_PID_MASK - digit) / 10u) {
            return 0;
        }
        pid = pid * 10u + digit;
        p++;
    }
    if (pid == 0u || sched_proc_task_by_pid(pid,
                                             &(sched_proc_task_info_t){0}) != 0) {
        return 0;
    }
    if (pid_out) {
        *pid_out = pid;
    }
    if (tail_out) {
        *tail_out = p;
    }
    return 1;
}

static void proc_buf_fixed3(proc_buffer_t *out, uint64_t milli_value) {
    proc_buf_u64(out, milli_value / 1000u);
    proc_buf_putc(out, '.');
    proc_buf_putc(out, (char)('0' + (milli_value / 100u) % 10u));
    proc_buf_putc(out, (char)('0' + (milli_value / 10u) % 10u));
    proc_buf_putc(out, (char)('0' + milli_value % 10u));
}

static void proc_buf_kib_line(proc_buffer_t *out, const char *name,
                              uint64_t bytes) {
    proc_buf_puts(out, name);
    proc_buf_puts(out, ": ");
    proc_buf_u64(out, bytes / 1024u);
    proc_buf_puts(out, " kB\n");
}

static uint32_t proc_runtime_available(void) {
    boot_info_t info;
    return vm_info_load_boot(&info) &&
           (info.features & BOOTINFO_FEATURE_RUNTIME_STATS) != 0u &&
           proc_read32(SYSINFO_MMIO_BASE + SYSINFO_REG_RUNTIME_VERSION) ==
               SYSINFO_RUNTIME_VERSION;
}

static uint32_t proc_lookup(const char *path, uint32_t *is_dir) {
    const char *name;
    const char *tail;
    uint32_t pid;
    if (!path) return 0u;
    if (proc_str_eq(path, "/proc") || proc_str_eq(path, "/proc/")) {
        if (is_dir) *is_dir = 1u;
        return PROC_INO_ROOT;
    }
    if (!fs_proc_path_match(path)) return 0u;
    if (proc_parse_task_path(path, &pid, &tail)) {
        if (*tail == '\0' || proc_str_eq(tail, "/")) {
            if (is_dir) *is_dir = 1u;
            return PROC_INO_TASK_DIR | pid;
        }
        if (proc_str_eq(tail, "/stat")) {
            if (is_dir) *is_dir = 0u;
            return PROC_INO_TASK_STAT | pid;
        }
        return 0u;
    }
    name = path + 6u;
    if (*name == '\0') {
        if (is_dir) *is_dir = 1u;
        return PROC_INO_ROOT;
    }
    for (uint32_t i = 2u;
         i < (uint32_t)(sizeof(g_proc_entries) / sizeof(g_proc_entries[0]));
         i++) {
        if (proc_str_eq(name, g_proc_entries[i].name)) {
            if (is_dir) *is_dir = 0u;
            return g_proc_entries[i].ino;
        }
    }
    return 0u;
}

static void proc_build_cpuinfo(proc_buffer_t *out) {
    boot_info_t boot;
    uint64_t frequency_hz = 0u;
    uint32_t cores = 1u;
    uint32_t timer_hz = 0u;
    if (vm_info_load_boot(&boot)) {
        if (boot.smp_cores != 0u) cores = boot.smp_cores;
        timer_hz = boot.timer_freq_hz;
    }
    if (proc_runtime_available()) {
        frequency_hz = proc_read64(SYSINFO_REG_CPU_FREQ_HZ_LO,
                                   SYSINFO_REG_CPU_FREQ_HZ_HI);
    }
    for (uint32_t cpu = 0u; cpu < cores; cpu++) {
        proc_buf_puts(out, "processor\t: ");
        proc_buf_u64(out, cpu);
        proc_buf_puts(out, "\nvendor_id\t: LampVM\nmodel name\t: LampVM LAMP32 virtual CPU\n");
        proc_buf_puts(out, "cpu MHz\t\t: ");
        proc_buf_fixed3(out, frequency_hz / 1000u);
        proc_buf_puts(out, "\ncycle model\t: one-cycle-per-instruction v1\ntimer Hz\t: ");
        proc_buf_u64(out, timer_hz);
        proc_buf_puts(out, "\nflags\t\t: smp mmu iommu pcie msi\n\n");
    }
}

static void proc_memory_values(uint64_t *total_out, uint64_t *used_out,
                               uint64_t *available_out,
                               sched_stats_t *sched_out) {
    extern uint8_t __kernel_end[];
    boot_info_t boot;
    sched_stats_t stats;
    uint64_t total = KERNEL_MEM_SIZE;
    uint64_t kernel_reserved =
        ((uint64_t)(uintptr_t)__kernel_end + 4095u) & ~4095ull;
    uint64_t used;
    sched_stats_snapshot(&stats);
    if (vm_info_load_boot(&boot) && boot.mem_bytes_hi == 0u) {
        total = boot.mem_bytes_lo;
    }
    used = kernel_reserved + stats.stack_bytes;
    if (stats.user_tasks != 0u) {
        used += USER_REGION_SIZE;
    }
    if (used > total) used = total;
    if (total_out) *total_out = total;
    if (used_out) *used_out = used;
    if (available_out) *available_out = total - used;
    if (sched_out) *sched_out = stats;
}

void fs_proc_memory_snapshot(uint64_t *total_out, uint64_t *used_out,
                             uint64_t *available_out) {
    proc_memory_values(total_out, used_out, available_out, 0);
}

static void proc_build_meminfo(proc_buffer_t *out) {
    uint64_t total;
    uint64_t used;
    uint64_t available;
    sched_stats_t stats;
    proc_memory_values(&total, &used, &available, &stats);
    proc_buf_kib_line(out, "MemTotal", total);
    proc_buf_kib_line(out, "MemFree", total - used);
    proc_buf_kib_line(out, "MemAvailable", available);
    proc_buf_kib_line(out, "Buffers", 0u);
    proc_buf_kib_line(out, "Cached", 0u);
    proc_buf_kib_line(out, "SReclaimable", 0u);
    proc_buf_kib_line(out, "KernelStack", stats.stack_bytes);
}

static void proc_build_uptime(proc_buffer_t *out) {
    sched_stats_t stats;
    uint64_t uptime_ns = 0u;
    uint64_t idle_us;
    if (proc_runtime_available()) {
        uptime_ns = proc_read64(SYSINFO_REG_UPTIME_NS_LO,
                                SYSINFO_REG_UPTIME_NS_HI);
    }
    sched_stats_snapshot(&stats);
    idle_us = (uint64_t)stats.idle_ticks * sched_tick_period_us();
    proc_buf_u64(out, uptime_ns / 1000000000ull);
    proc_buf_putc(out, '.');
    proc_buf_putc(out, (char)('0' + (uptime_ns / 100000000ull) % 10u));
    proc_buf_putc(out, (char)('0' + (uptime_ns / 10000000ull) % 10u));
    proc_buf_putc(out, ' ');
    proc_buf_u64(out, idle_us / 1000000ull);
    proc_buf_putc(out, '.');
    proc_buf_putc(out, (char)('0' + (idle_us / 100000ull) % 10u));
    proc_buf_putc(out, (char)('0' + (idle_us / 10000ull) % 10u));
    proc_buf_putc(out, '\n');
}

static void proc_build_stat(proc_buffer_t *out) {
    sched_stats_t stats;
    boot_info_t boot;
    uint64_t boot_seconds = 0u;
    sched_stats_snapshot(&stats);
    if (vm_info_load_boot(&boot)) {
        boot_seconds = (((uint64_t)boot.boot_realtime_ns_hi << 32) |
                        boot.boot_realtime_ns_lo) / 1000000000ull;
    }
    proc_buf_puts(out, "cpu  ");
    proc_buf_u64(out, stats.user_ticks);
    proc_buf_puts(out, " 0 ");
    proc_buf_u64(out, stats.system_ticks);
    proc_buf_putc(out, ' ');
    proc_buf_u64(out, stats.idle_ticks);
    proc_buf_puts(out, " 0 0 0 0 0 0\n");
    proc_buf_puts(out, "btime ");
    proc_buf_u64(out, boot_seconds);
    proc_buf_puts(out, "\nprocesses ");
    proc_buf_u64(out, stats.task_count);
    proc_buf_puts(out, "\nprocs_running ");
    proc_buf_u64(out, stats.running_tasks + stats.runnable_tasks);
    proc_buf_puts(out, "\nprocs_blocked ");
    proc_buf_u64(out, stats.blocked_tasks);
    proc_buf_putc(out, '\n');
}

static void proc_build_loadavg(proc_buffer_t *out) {
    sched_stats_t stats;
    uint64_t runnable_milli;
    sched_stats_snapshot(&stats);
    runnable_milli =
        ((uint64_t)(stats.running_tasks + stats.runnable_tasks) * 1000u) /
        (stats.online_cpus ? stats.online_cpus : 1u);
    proc_buf_fixed3(out, runnable_milli);
    proc_buf_putc(out, ' ');
    proc_buf_fixed3(out, runnable_milli);
    proc_buf_putc(out, ' ');
    proc_buf_fixed3(out, runnable_milli);
    proc_buf_putc(out, ' ');
    proc_buf_u64(out, stats.running_tasks + stats.runnable_tasks);
    proc_buf_putc(out, '/');
    proc_buf_u64(out, stats.task_count);
    proc_buf_puts(out, " 0\n");
}

static void proc_build_lampvm(proc_buffer_t *out) {
    uint64_t total;
    uint64_t used;
    uint64_t available;
    sched_stats_t sched_stats;
    uint64_t frequency = 0u;
    uint64_t cycles = 0u;
    uint64_t executed = 0u;
    uint64_t rate = 0u;
    uint64_t uptime = 0u;
    uint64_t rss = 0u;
    proc_memory_values(&total, &used, &available, &sched_stats);
    if (proc_runtime_available()) {
        frequency = proc_read64(SYSINFO_REG_CPU_FREQ_HZ_LO,
                                SYSINFO_REG_CPU_FREQ_HZ_HI);
        cycles = proc_read64(SYSINFO_REG_CPU_CYCLES_LO,
                             SYSINFO_REG_CPU_CYCLES_HI);
        executed = proc_read64(SYSINFO_REG_EXEC_COUNT_LO,
                               SYSINFO_REG_EXEC_COUNT_HI);
        rate = proc_read64(SYSINFO_REG_EXEC_RATE_HZ_LO,
                           SYSINFO_REG_EXEC_RATE_HZ_HI);
        uptime = proc_read64(SYSINFO_REG_UPTIME_NS_LO,
                             SYSINFO_REG_UPTIME_NS_HI);
        rss = proc_read64(SYSINFO_REG_HOST_RSS_BYTES_LO,
                          SYSINFO_REG_HOST_RSS_BYTES_HI);
    }
    proc_buf_puts(out, "clock_model paced-monotonic-v1\ncycle_model one-cycle-per-instruction-v1\n");
    proc_buf_puts(out, "cpu_frequency_hz "); proc_buf_u64(out, frequency);
    proc_buf_puts(out, "\nvirtual_cycles "); proc_buf_u64(out, cycles);
    proc_buf_puts(out, "\nexecuted_instructions "); proc_buf_u64(out, executed);
    proc_buf_puts(out, "\nexecution_rate_ips "); proc_buf_u64(out, rate);
    proc_buf_puts(out, "\nuptime_ns "); proc_buf_u64(out, uptime);
    proc_buf_puts(out, "\nguest_ram_bytes "); proc_buf_u64(out, total);
    proc_buf_puts(out, "\nguest_estimated_used_bytes "); proc_buf_u64(out, used);
    proc_buf_puts(out, "\nguest_available_bytes "); proc_buf_u64(out, available);
    proc_buf_puts(out, "\nhost_rss_bytes "); proc_buf_u64(out, rss);
    proc_buf_puts(out, "\ntasks "); proc_buf_u64(out, sched_stats.task_count);
    proc_buf_putc(out, '\n');
}

static uint32_t proc_build(uint32_t ino, char *dst, uint32_t capacity) {
    proc_buffer_t out;
    if (!dst || capacity == 0u) return 0u;
    out.data = dst;
    out.capacity = capacity;
    out.length = 0u;
    out.data[0] = '\0';
    if (proc_is_task_stat(ino)) {
        return proc_task_stat(proc_task_pid(ino), dst, capacity);
    }
    switch (ino) {
        case PROC_INO_CPUINFO: proc_build_cpuinfo(&out); break;
        case PROC_INO_MEMINFO: proc_build_meminfo(&out); break;
        case PROC_INO_UPTIME: proc_build_uptime(&out); break;
        case PROC_INO_STAT: proc_build_stat(&out); break;
        case PROC_INO_VERSION:
            proc_buf_puts(&out, "Lamp kernel 0.31 (lamp32) procfs-v1\n");
            break;
        case PROC_INO_LOADAVG: proc_build_loadavg(&out); break;
        case PROC_INO_LAMPVM: proc_build_lampvm(&out); break;
        default: break;
    }
    return out.length;
}

uint32_t fs_proc_path_match(const char *path) {
    return path && path[0] == '/' && path[1] == 'p' && path[2] == 'r' &&
           path[3] == 'o' && path[4] == 'c' &&
           (path[5] == '\0' || path[5] == '/') ? 1u : 0u;
}

int fs_proc_open(const char *path, uint32_t flags) {
    char scratch[PROC_CONTENT_CAP];
    uint32_t is_dir = 0u;
    uint32_t ino = proc_lookup(path, &is_dir);
    uint32_t size;
    uint32_t acc = flags & SYS_O_ACCMODE;
    if (ino == 0u) return FS_ERR_NOENT;
    if (acc != SYS_O_RDONLY ||
        (flags & (SYS_O_CREAT | SYS_O_TRUNC | SYS_O_APPEND)) != 0u) {
        return FS_ERR_ROFS;
    }
    size = is_dir ? proc_directory_size(ino) :
        proc_build(ino, scratch, (uint32_t)sizeof(scratch));
    return sched_fd_open_regular(flags & (SYS_O_ACCMODE | SYS_O_NONBLOCK),
                                 FS_BACKEND_PROC, ino, size, is_dir);
}

int fs_proc_stat(const char *path, fs_stat_t *st) {
    char scratch[PROC_CONTENT_CAP];
    uint32_t is_dir = 0u;
    uint32_t ino = proc_lookup(path, &is_dir);
    if (!st) return FS_ERR_INVAL;
    if (ino == 0u) return FS_ERR_NOENT;
    st->st_dev = FS_BACKEND_PROC;
    st->st_ino = ino;
    st->st_mode = (is_dir ? SYS_S_IFDIR | 0555u : SYS_S_IFREG | 0444u);
    st->st_nlink = is_dir ? 2u : 1u;
    st->st_uid = 0u;
    st->st_gid = 0u;
    st->st_rdev = 0u;
    st->st_size = is_dir ? proc_directory_size(ino) :
        proc_build(ino, scratch, (uint32_t)sizeof(scratch));
    st->st_blksize = 4096u;
    st->st_blocks = (st->st_size + 511u) / 512u;
    return 0;
}

int fs_proc_getdents_fd(int32_t fd, fs_dirent_t *dst, uint32_t len) {
    uint32_t backend;
    uint32_t ino;
    uint32_t offset;
    uint32_t is_dir;
    const proc_entry_t *entry;
    sched_proc_task_info_t task;
    uint32_t i;
    if (!dst || len < (uint32_t)sizeof(*dst)) return FS_ERR_INVAL;
    if (sched_fd_regular_get(fd, &backend, &ino, 0, &offset, &is_dir) !=
        SCHED_FD_OK) return FS_ERR_BADF;
    if (backend != FS_BACKEND_PROC) return FS_ERR_BADF;
    if (!is_dir) return FS_ERR_NOTDIR;

    if (proc_is_task_dir(ino)) {
        static const proc_entry_t task_entries[] = {
            { 0u, ".", SYS_DT_DIR },
            { PROC_INO_ROOT, "..", SYS_DT_DIR },
            { 0u, "stat", SYS_DT_REG }
        };
        const uint32_t pid = proc_task_pid(ino);
        if (sched_proc_task_by_pid(pid, &task) != 0 ||
            offset >= (uint32_t)(sizeof(task_entries) / sizeof(task_entries[0]))) {
            return 0;
        }
        entry = &task_entries[offset];
        dst->d_ino = entry->ino;
        if (offset == 0u) dst->d_ino = ino;
        if (offset == 2u) dst->d_ino = PROC_INO_TASK_STAT | pid;
        dst->d_off = offset + 1u;
        dst->d_reclen = (uint32_t)sizeof(*dst);
        dst->d_type = entry->type;
        for (i = 0u; i + 1u < (uint32_t)sizeof(dst->d_name) &&
                      entry->name[i] != '\0'; i++) {
            dst->d_name[i] = entry->name[i];
        }
        dst->d_name[i] = '\0';
        (void)sched_fd_regular_advance(fd, 1u, 0);
        return (int)sizeof(*dst);
    }

    if (ino != PROC_INO_ROOT) return FS_ERR_BADF;
    for (i = offset + 1u; i < SCHED_MAX_TASKS; i++) {
        if (sched_proc_task_by_slot(i, &task) != 0) {
            continue;
        }
        dst->d_ino = PROC_INO_TASK_DIR | task.pid;
        dst->d_off = i;
        dst->d_reclen = (uint32_t)sizeof(*dst);
        dst->d_type = SYS_DT_DIR;
        {
            proc_buffer_t name_out = {
                .data = dst->d_name,
                .capacity = (uint32_t)sizeof(dst->d_name),
                .length = 0u
            };
            dst->d_name[0] = '\0';
            proc_buf_u64(&name_out, task.pid);
        }
        (void)sched_fd_regular_advance(fd, dst->d_off - offset, 0);
        return (int)sizeof(*dst);
    }

    if (offset < SCHED_MAX_TASKS + (uint32_t)(sizeof(g_proc_entries) / sizeof(g_proc_entries[0]))) {
        uint32_t static_index = offset < SCHED_MAX_TASKS ? 0u : offset - SCHED_MAX_TASKS;
        entry = &g_proc_entries[static_index];
        dst->d_ino = entry->ino;
        dst->d_off = SCHED_MAX_TASKS + static_index + 1u;
        dst->d_reclen = (uint32_t)sizeof(*dst);
        dst->d_type = entry->type;
        for (i = 0u; i + 1u < (uint32_t)sizeof(dst->d_name) &&
                      entry->name[i] != '\0'; i++) {
            dst->d_name[i] = entry->name[i];
        }
        dst->d_name[i] = '\0';
        (void)sched_fd_regular_advance(fd, dst->d_off - offset, 0);
        return (int)sizeof(*dst);
    }

    return 0;
}

int fs_proc_read_fd(int32_t fd, uint8_t *dst, uint32_t len) {
    char content[PROC_CONTENT_CAP];
    uint32_t backend;
    uint32_t ino;
    uint32_t offset;
    uint32_t is_dir;
    uint32_t size;
    if (!dst || len == 0u) return 0;
    if (sched_fd_regular_get(fd, &backend, &ino, 0, &offset, &is_dir) !=
        SCHED_FD_OK) return FS_ERR_BADF;
    if (backend != FS_BACKEND_PROC) return FS_ERR_BADF;
    if (is_dir) return FS_ERR_ISDIR;
    size = proc_build(ino, content, (uint32_t)sizeof(content));
    if (offset >= size) return 0;
    if (len > size - offset) len = size - offset;
    for (uint32_t i = 0u; i < len; i++) {
        dst[i] = (uint8_t)content[offset + i];
    }
    (void)sched_fd_regular_advance(fd, len, 0);
    return (int)len;
}
