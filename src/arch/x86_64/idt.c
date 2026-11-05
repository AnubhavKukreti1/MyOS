/*
 * MyOS - src/arch/x86_64/idt.c
 * 256 gates. Exceptions panic unless handled; IRQs are acknowledged after
 * their handler; the resched hook decides which context to resume.
 */

#include "kernel.h"
#include "arch/x86_64/idt.h"

typedef struct __attribute__((packed)) {
    u16 offset_lo;
    u16 selector;
    u8  ist;
    u8  type_attr;
    u16 offset_mid;
    u32 offset_hi;
    u32 reserved;
} idt_entry_t;

#define IDT_GATE_INTERRUPT 0x8E    /* present, DPL0, 64-bit interrupt gate */
#define IDT_GATE_USER      0xEE    /* same, DPL3: reachable with `int` from ring 3 */

static idt_entry_t    g_idt[IDT_VECTORS] __attribute__((aligned(16)));
static exc_handler_t  g_handlers[IDT_VECTORS];
static resched_hook_t g_resched_hook;
static exc_handler_t  g_user_fault;

extern const uintptr_t isr_stub_table[IDT_VECTORS];

static const char *const k_exc_names[EXC_VECTORS] = {
    "Divide Error",            "Debug",                    "NMI",
    "Breakpoint",              "Overflow",                 "BOUND Range Exceeded",
    "Invalid Opcode",          "Device Not Available",     "Double Fault",
    "Coprocessor Overrun",     "Invalid TSS",              "Segment Not Present",
    "Stack-Segment Fault",     "General Protection",       "Page Fault",
    "Reserved",                "x87 Floating-Point",       "Alignment Check",
    "Machine Check",           "SIMD Floating-Point",      "Virtualization",
    "Control Protection",      "Reserved",                 "Reserved",
    "Reserved",                "Reserved",                 "Reserved",
    "Reserved",                "Hypervisor Injection",     "VMM Communication",
    "Security Exception",      "Reserved",
};

static void idt_set_gate(u32 vector, uintptr_t handler, u8 ist, u8 attr) {
    idt_entry_t *e = &g_idt[vector];

    e->offset_lo  = (u16)(handler & 0xFFFF);
    e->selector   = GDT_KERNEL_CS;
    e->ist        = ist;
    e->type_attr  = attr;
    e->offset_mid = (u16)((handler >> 16) & 0xFFFF);
    e->offset_hi  = (u32)(handler >> 32);
    e->reserved   = 0;
}

void idt_init(void) {
    for (u32 v = 0; v < IDT_VECTORS; v++) {
        idt_set_gate(v, isr_stub_table[v],
                     (v == 8) ? 1 : 0,                     /* #DF runs on IST1 */
                     (v == SYSCALL_VECTOR) ? IDT_GATE_USER : IDT_GATE_INTERRUPT);
    }
    dtr_t idtr = { .limit = (u16)(sizeof g_idt - 1), .base = (u64)(uintptr_t)g_idt };
    __asm__ volatile ("lidt %0" : : "m"(idtr) : "memory");
}

bool idt_register_handler(u8 vector, exc_handler_t handler) {
    g_handlers[vector] = handler;
    return true;
}

void idt_set_resched_hook(resched_hook_t h)        { g_resched_hook = h; }
void idt_set_user_fault_handler(exc_handler_t h)   { g_user_fault = h; }

static NORETURN void exception_fatal(interrupt_frame_t *f) {
    const char *name = (f->vector < EXC_VECTORS) ? k_exc_names[f->vector] : "Unknown";

    if (f->vector == 14) {
        u64 err = f->error_code;
        char detail[128];

        ksnprintf(detail, sizeof detail, "%s while %s at %p (%s mode)%s%s",
                  (err & 1) ? "protection violation" : "page not present",
                  (err & 2) ? "writing" : "reading",
                  (void *)(uintptr_t)read_cr2(),
                  (err & 4) ? "user" : "kernel",
                  (err & 8) ? " [reserved bit set]" : "",
                  (err & 16) ? " [instruction fetch]" : "");
        kpanic_frame(f, "Page Fault: %s", detail);
    }
    kpanic_frame(f, "Unhandled exception #%lu (%s), error code 0x%lx",
                 f->vector, name, f->error_code);
}

interrupt_frame_t *exception_dispatch(interrupt_frame_t *f) {
    const u64 v = f->vector;
    exc_handler_t h = (v < IDT_VECTORS) ? g_handlers[v] : NULL;

    if (v < EXC_VECTORS) {
        if (h != NULL) {
            h(f);
        } else if ((f->cs & 3) == 3 && g_user_fault != NULL) {
            g_user_fault(f);               /* kill the process, keep the kernel */
        } else {
            exception_fatal(f);
        }
    } else if (v >= IRQ_BASE && v < IRQ_BASE + 16) {
        u8 irq = (u8)(v - IRQ_BASE);
        if (pic_spurious(irq)) {
            return f;
        }
        if (h != NULL) {
            h(f);
        }
        pic_eoi(irq);                        /* BEFORE any task switch */
    } else if (h != NULL) {
        h(f);
    } else {
        kpanic_frame(f, "Unhandled interrupt vector %lu", v);
    }

    return (g_resched_hook != NULL) ? g_resched_hook(f) : f;
}