#ifndef MYOS_KERNEL_SYSCALL_H
#define MYOS_KERNEL_SYSCALL_H

#include "kernel.h"

/* ABI: int $0x80, number in rax, args in rdi/rsi/rdx, result in rax (negative = -errno). */
#define SYS_WRITE     1     /* (fd 1|2, buf, len) -> bytes written */
#define SYS_YIELD     2
#define SYS_EXIT      3     /* (code) */
#define SYS_SLEEP_MS  4     /* (ms) */
#define SYS_GETPID    5

void syscall_init(void);

#endif