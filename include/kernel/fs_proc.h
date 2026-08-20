#ifndef LAMP_KERNEL_FS_PROC_H
#define LAMP_KERNEL_FS_PROC_H

#include "fs.h"
#include "types.h"

uint32_t fs_proc_path_match(const char *path);
int fs_proc_open(const char *path, uint32_t flags);
int fs_proc_stat(const char *path, fs_stat_t *st);
int fs_proc_getdents_fd(int32_t fd, fs_dirent_t *dst, uint32_t len);
int fs_proc_read_fd(int32_t fd, uint8_t *dst, uint32_t len);
void fs_proc_memory_snapshot(uint64_t *total_out, uint64_t *used_out,
                             uint64_t *available_out);

#endif
