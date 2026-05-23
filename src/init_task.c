#include "../include/kernel/fs.h"
#include "../include/kernel/init_task.h"
#include "../include/kernel/printk.h"
#include "../include/kernel/sched.h"
#include "../include/kernel/types.h"
#include "../include/kernel/user_exec.h"

enum {
    INIT_RESPAWN_DELAY_TICKS = 50u
};

static const char *const g_busybox_argv[] = {
    "-/bin/sh",
    0
};

static const char *const g_busybox_envp[] = {
    "PATH=/bin",
    "HOME=/",
    "SHELL=/bin/sh",
    0
};

static void init_log_spawn_failed(int32_t rc) {
    klog_begin(KLOG_LEVEL_ERROR, "init");
    klog_puts("failed to spawn /bin/sh rc=");
    klog_hex32((uint32_t)rc);
    if (rc == FS_ERR_NOENT) {
        klog_puts(" missing BusyBox shell");
    }
    klog_end();
}

static int init_wait_child(uint32_t *status_out) {
    uint32_t status = 0u;
    for (;;) {
        int rc = sched_waitpid(SCHED_WAITPID_ANY, 0u, &status);
        if (rc == SCHED_WAITPID_BLOCKED) {
            sched_block_until_runnable();
            continue;
        }
        if (rc <= 0) {
            if (status_out) {
                *status_out = 0x7F00u;
            }
            return rc;
        }
        if (status_out) {
            *status_out = status;
        }
        return rc;
    }
}

static void init_task_entry(sched_task_t *task, void *arg) {
    (void)task;
    (void)arg;

    for (;;) {
        uint32_t status = 0u;
        int32_t tid = user_exec_spawn_path("/bin/sh", g_busybox_argv, g_busybox_envp);
        if (tid < 0) {
            init_log_spawn_failed(tid);
            sched_sleep_ticks(INIT_RESPAWN_DELAY_TICKS);
            continue;
        }

        KLOGI("init", "busybox spawned");
        if (init_wait_child(&status) <= 0) {
            KLOGW("init", "busybox wait failed");
        } else {
            klog_begin(KLOG_LEVEL_WARN, "init");
            klog_puts("busybox exited status=");
            klog_hex32((status >> 8u) & 0xFFu);
            klog_end();
        }
        sched_sleep_ticks(INIT_RESPAWN_DELAY_TICKS);
    }
}

void init_task_spawn(void) {
    if (sched_spawn("init", init_task_entry, 0) < 0) {
        KLOGW("init", "spawn failed");
    } else {
        KLOGI("init", "spawned");
    }
}
