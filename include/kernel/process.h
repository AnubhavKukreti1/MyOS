#ifndef MYOS_KERNEL_PROCESS_H
#define MYOS_KERNEL_PROCESS_H

#include "kernel.h"
#include "kernel/scheduler.h"

#define USER_CODE_BASE   0x0000000000400000ULL
#define USER_STACK_TOP   0x00007FFFFFFFF000ULL
#define USER_STACK_PAGES 4

/* Copies `image` to USER_CODE_BASE (RX), maps a stack, starts one thread at
 * USER_CODE_BASE + entry_offset. Returns NULL on failure (nothing leaks). */
process_t *process_create_user(const char *name, const void *image, size_t size, u64 entry_offset);
void       process_destroy(process_t *p);

#endif