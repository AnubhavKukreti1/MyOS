#ifndef MYOS_ARCH_IDT_H
#define MYOS_ARCH_IDT_H

#include "kernel.h"

#define IRQ_BASE            0x20      /* PIC remapped to vectors 0x20..0x2F */
#define SYSCALL_VECTOR      0x80      /* int 0x80, callable from ring 3     */
#define SCHED_YIELD_VECTOR  0x81      /* int 0x81, ring 0 only              */

/* Called after every dispatch; may return a different frame to switch tasks. */
typedef interrupt_frame_t *(*resched_hook_t)(interrupt_frame_t *);
void idt_set_resched_hook(resched_hook_t h);
/* Called for unhandled exceptions raised from ring 3. */
void idt_set_user_fault_handler(exc_handler_t h);

/* 8259 PIC */
void pic_init(void);
void pic_eoi(u8 irq);
bool pic_spurious(u8 irq);
void irq_mask(u8 irq);
void irq_unmask(u8 irq);
bool irq_register(u8 irq, exc_handler_t handler);   /* registers + unmasks */

#endif