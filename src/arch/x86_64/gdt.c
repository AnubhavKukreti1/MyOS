/*
 * MyOS - src/arch/x86_64/gdt.c
 * Order is fixed by SYSCALL/SYSRET conventions: kernel CS/DS, user DS before
 * user CS. TSS (16-byte descriptor) at 0x28: rsp0 for ring3->ring0 entry, IST1
 * for the double-fault handler.
 */

#include "kernel.h"

typedef struct __attribute__((packed)) {
    u32 reserved0;
    u64 rsp0, rsp1, rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} tss_t;

static tss_t g_tss;
static u8    g_ist1_stack[16 * 1024] __attribute__((aligned(16)));

static u64 g_gdt[7] __attribute__((aligned(16))) = {
    0x0000000000000000ULL,    /* 0x00 null                 */
    0x00AF9A000000FFFFULL,    /* 0x08 kernel code          */
    0x00CF92000000FFFFULL,    /* 0x10 kernel data          */
    0x00CFF2000000FFFFULL,    /* 0x18 user data   (DPL3)   */
    0x00AFFA000000FFFFULL,    /* 0x20 user code   (DPL3)   */
    0, 0,                     /* 0x28 TSS (filled in below)*/
};

void tss_set_rsp0(u64 rsp0) {
    g_tss.rsp0 = rsp0;
}

void gdt_init(void) {
    u64 base  = (u64)(uintptr_t)&g_tss;
    u64 limit = sizeof g_tss - 1;

    g_tss.iomap_base = (u16)sizeof g_tss;                 /* no I/O permission bitmap */
    g_tss.ist[0]     = (u64)(uintptr_t)(g_ist1_stack + sizeof g_ist1_stack);

    g_gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
               ((limit & 0xF0000) << 32) | ((base & 0xFF000000ULL) << 32);
    g_gdt[6] = base >> 32;

    dtr_t gdtr = { .limit = (u16)(sizeof g_gdt - 1), .base = (u64)(uintptr_t)g_gdt };
    __asm__ volatile ("lgdt %0" : : "m"(gdtr) : "memory");

    __asm__ volatile (
        "pushq %[cs]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw %[ds], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        :
        : [cs] "i"(GDT_KERNEL_CS), [ds] "i"(GDT_KERNEL_DS)
        : "rax", "memory");

    __asm__ volatile ("ltr %w0" : : "r"((u16)GDT_TSS));
}