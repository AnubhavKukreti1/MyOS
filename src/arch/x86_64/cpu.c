/*
 * MyOS - src/arch/x86_64/cpu.c
 * Part of the MyOS 0.2.0 kernel (split out of the monolithic main.c).
 */

/* #########################################################################
   CPU CHECKS
   -------------------------------------------------------------------------
   Limine hands us a CPU already in 64-bit long mode with paging enabled, so
   these checks are belt-and-braces: they document our assumptions, surface
   hardware features the PMM/VMM will want, and turn a violated assumption
   into a clear panic instead of a mystery crash three subsystems later.
   ######################################################################### */

#include "kernel.h"

void cpu_early_init(cpu_info_t *ci) {
    u32 a, b, c, d;

    memset(ci, 0, sizeof *ci);

    cpuid(0, 0, &a, &b, &c, &d);
    ci->max_basic = a;
    memcpy(&ci->vendor[0], &b, 4);   /* vendor string order: EBX, EDX, ECX */
    memcpy(&ci->vendor[4], &d, 4);
    memcpy(&ci->vendor[8], &c, 4);

    cpuid(0x80000000u, 0, &a, &b, &c, &d);
    ci->max_ext = (a >= 0x80000000u) ? a : 0;

    if (ci->max_basic >= 1) {
        cpuid(1, 0, &a, &b, &c, &d);
        ci->apic   = (d >> 9)  & 1;
        ci->pat    = (d >> 16) & 1;
        ci->sse2   = (d >> 26) & 1;
        ci->x2apic = (c >> 21) & 1;
    }
    if (ci->max_ext >= 0x80000001u) {
        cpuid(0x80000001u, 0, &a, &b, &c, &d);
        ci->nx        = (d >> 20) & 1;
        ci->page1g    = (d >> 26) & 1;
        ci->long_mode = (d >> 29) & 1;
    }
    if (ci->max_ext >= 0x80000007u) {
        cpuid(0x80000007u, 0, &a, &b, &c, &d);
        ci->invariant_tsc = (d >> 8) & 1;
    }
    if (ci->max_ext >= 0x80000008u) {
        cpuid(0x80000008u, 0, &a, &b, &c, &d);
        ci->phys_bits = (u8)(a & 0xFF);
        ci->virt_bits = (u8)((a >> 8) & 0xFF);
    } else {
        ci->phys_bits = 36;   /* architectural minimum assumption */
        ci->virt_bits = 48;
    }
    if (ci->max_ext >= 0x80000004u) {
        for (u32 i = 0; i < 3; i++) {
            cpuid(0x80000002u + i, 0, &a, &b, &c, &d);
            memcpy(&ci->brand[i * 16 + 0],  &a, 4);
            memcpy(&ci->brand[i * 16 + 4],  &b, 4);
            memcpy(&ci->brand[i * 16 + 8],  &c, 4);
            memcpy(&ci->brand[i * 16 + 12], &d, 4);
        }
    }

    if (!ci->long_mode) {
        kpanic("CPU does not report long mode support (CPUID 0x80000001)");
    }

    ci->cr0  = read_cr0();
    ci->cr3  = read_cr3();
    ci->cr4  = read_cr4();
    ci->efer = rdmsr(MSR_EFER);

    if (!(ci->cr0 & CR0_PE) || !(ci->cr0 & CR0_PG) ||
        !(ci->cr4 & CR4_PAE) || !(ci->efer & EFER_LMA)) {
        kpanic("Unexpected CPU state: CR0=%lx CR4=%lx EFER=%lx "
               "(need PE+PG, PAE, LMA)", ci->cr0, ci->cr4, ci->efer);
    }
}

void cpu_report(const cpu_info_t *ci) {
    const char *brand = ci->brand;
    while (*brand == ' ') {
        brand++;
    }

    kok("CPU is in 64-bit long mode (EFER.LMA, CR0.PG, CR4.PAE set)");
    kinfo("CPU vendor : %s", ci->vendor);
    kinfo("CPU model  : %s", (*brand != '\0') ? brand : "(unavailable)");
    kinfo("Address bits: %u physical / %u virtual", (unsigned)ci->phys_bits,
          (unsigned)ci->virt_bits);
    kinfo("Features   : NX=%s 1GiB-pages=%s SSE2=%s APIC=%s x2APIC=%s PAT=%s "
          "invariant-TSC=%s",
          ci->nx ? "yes" : "no", ci->page1g ? "yes" : "no",
          ci->sse2 ? "yes" : "no", ci->apic ? "yes" : "no",
          ci->x2apic ? "yes" : "no", ci->pat ? "yes" : "no",
          ci->invariant_tsc ? "yes" : "no");
    kinfo("Control    : CR0=0x%lx CR3=0x%lx CR4=0x%lx EFER=0x%lx",
          ci->cr0, ci->cr3, ci->cr4, ci->efer);

    if (!ci->nx || !(ci->efer & EFER_NXE)) {
        kwarn("NX not active: W^X page protection will be unavailable");
    }
    if (!(ci->cr0 & CR0_WP)) {
        kwarn("CR0.WP clear: ring 0 can write read-only pages");
    }
}