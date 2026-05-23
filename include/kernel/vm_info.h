#ifndef LAMP_KERNEL_VM_INFO_H
#define LAMP_KERNEL_VM_INFO_H

#include "types.h"

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t vendor_words[4];
    uint32_t mem_bytes_lo;
    uint32_t mem_bytes_hi;
    uint32_t disk_bytes_lo;
    uint32_t disk_bytes_hi;
    uint32_t smp_cores;
    uint32_t layout_version;
    uint32_t arch_id;
    uint32_t endian;
    uint32_t phys_addr_bits;
    uint32_t page_size;
    uint32_t timer_freq_hz;
    uint32_t features;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_bpp;
    uint32_t fb_stride_bytes;
    uint32_t boot_realtime_ns_lo;
    uint32_t boot_realtime_ns_hi;
} boot_info_t;

int vm_info_load_boot(boot_info_t *out);
void vm_info_log_boot(void);

#endif
