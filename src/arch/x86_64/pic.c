/* MyOS - src/arch/x86_64/pic.c : legacy 8259 PIC */

#include "kernel.h"
#include "arch/x86_64/idt.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

static void io_wait(void) { outb(0x80, 0); }

void pic_init(void) {
    outb(PIC1_CMD, 0x11);  io_wait();            /* ICW1: init, expect ICW4 */
    outb(PIC2_CMD, 0x11);  io_wait();
    outb(PIC1_DATA, IRQ_BASE);      io_wait();   /* ICW2: vector offsets    */
    outb(PIC2_DATA, IRQ_BASE + 8);  io_wait();
    outb(PIC1_DATA, 4);    io_wait();            /* ICW3: slave on IRQ2     */
    outb(PIC2_DATA, 2);    io_wait();
    outb(PIC1_DATA, 0x01); io_wait();            /* ICW4: 8086 mode         */
    outb(PIC2_DATA, 0x01); io_wait();

    outb(PIC1_DATA, 0xFB);                       /* mask everything except the cascade */
    outb(PIC2_DATA, 0xFF);
}

void irq_mask(u8 irq) {
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    outb(port, (u8)(inb(port) | (1u << (irq & 7))));
}

void irq_unmask(u8 irq) {
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    outb(port, (u8)(inb(port) & ~(1u << (irq & 7))));
}

void pic_eoi(u8 irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, 0x20);
    }
    outb(PIC1_CMD, 0x20);
}

/* IRQ 7 / 15 can fire spuriously; the in-service register tells the truth. */
bool pic_spurious(u8 irq) {
    if (irq != 7 && irq != 15) {
        return false;
    }
    u16 cmd = (irq == 7) ? PIC1_CMD : PIC2_CMD;
    outb(cmd, 0x0B);                             /* OCW3: read ISR */
    if (inb(cmd) & 0x80) {
        return false;
    }
    if (irq == 15) {
        outb(PIC1_CMD, 0x20);                    /* the cascade line was real */
    }
    return true;
}

bool irq_register(u8 irq, exc_handler_t handler) {
    if (irq >= 16) {
        return false;
    }
    idt_register_handler((u8)(IRQ_BASE + irq), handler);
    irq_unmask(irq);
    return true;
}