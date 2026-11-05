/* MyOS - src/kernel/syscall.c */

#include "kernel.h"
#include "arch/x86_64/idt.h"
#include "mm/vmm.h"
#include "kernel/scheduler.h"
#include "kernel/syscall.h"

#define E_BADF   9
#define E_FAULT 14
#define E_NOSYS 38

static i64 sys_write(u64 fd, u64 buf, u64 len) {
    if (fd != 1 && fd != 2) {
        return -E_BADF;
    }
    if (len > 4096) {
        len = 4096;
    }
    task_t *t = sched_current();
    if (t->proc != NULL && !vmm_user_range_ok(t->proc->as, buf, len, false)) {
        return -E_FAULT;                     /* never trust a user pointer */
    }
    /* We are running in the caller's address space, so the validated user
     * addresses are directly readable. */
    char chunk[128];
    for (u64 done = 0; done < len; ) {
        size_t n = (len - done < sizeof chunk) ? (size_t)(len - done) : sizeof chunk;
        memcpy(chunk, (const void *)(uintptr_t)(buf + done), n);
        for (size_t i = 0; i < n; i++) {
            kputc(chunk[i]);
        }
        done += n;
    }
    return (i64)len;
}

static void syscall_handler(interrupt_frame_t *f) {
    i64 ret = -E_NOSYS;

    switch (f->rax) {
    case SYS_WRITE:    ret = sys_write(f->rdi, f->rsi, f->rdx);   break;
    case SYS_YIELD:    ret = 0; sched_isr_yield();                break;
    case SYS_EXIT:     ret = 0; sched_isr_exit((int)f->rdi);      break;
    case SYS_SLEEP_MS: ret = 0; sched_isr_sleep((u32)f->rdi);     break;
    case SYS_GETPID: {
        process_t *p = sched_current()->proc;
        ret = (p != NULL) ? (i64)p->pid : 0;
        break;
    }
    default:
        break;
    }
    f->rax = (u64)ret;
}

void syscall_init(void) {
    idt_register_handler(SYSCALL_VECTOR, syscall_handler);
    kok("Syscalls: int 0x%x (write, yield, exit, sleep_ms, getpid)", SYSCALL_VECTOR);
}