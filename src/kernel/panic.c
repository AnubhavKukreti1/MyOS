/*
 * MyOS - src/kernel/panic.c
 * Part of the MyOS 0.2.0 kernel (split out of the monolithic main.c).
 */

/* #########################################################################
   PANIC
   -------------------------------------------------------------------------
   Design rules for a panic path:
     * Never allocate, never take locks, never trust anything but the stack.
     * Interrupts off first; a second panic while panicking just halts.
     * Output goes to EVERY sink regardless of the log level filter.
     * The backtrace walker validates each frame pointer before touching it,
       because a fault inside the panic handler would escalate to a double
       fault and then a silent triple fault (reboot loop, no message).
   Symbolisation (address -> function name) is a future step: embed a symbol
   table or post-process the serial log with `addr2line -e kernel.elf`.
   ######################################################################### */

#include "kernel.h"

static volatile bool g_panicking = false;

#define BACKTRACE_MAX_FRAMES 16
#define BACKTRACE_MAX_STRIDE 0x10000ULL   /* heuristic: sane stack frames */

static bool stack_ptr_plausible(u64 p) {
    return p >= HIGHER_HALF_START && (p & 7) == 0 && p < (u64)-16;
}

static void print_backtrace(u64 rbp, u64 rip) {
    kprintf("\n Backtrace (needs -fno-omit-frame-pointer):\n");

    unsigned depth = 0;
    if (rip != 0) {
        kprintf("   #%02u  %p  (faulting instruction)\n", depth++, (void *)(uintptr_t)rip);
    }

    while (depth < BACKTRACE_MAX_FRAMES && stack_ptr_plausible(rbp)) {
        const u64 *fp = (const u64 *)(uintptr_t)rbp;
        u64 ret  = fp[1];
        u64 next = fp[0];

        if (ret == 0) {
            break;
        }
        kprintf("   #%02u  %p\n", depth++, (void *)(uintptr_t)ret);

        if (next <= rbp || next - rbp > BACKTRACE_MAX_STRIDE) {
            break;   /* corrupt or end of chain */
        }
        rbp = next;
    }
    if (depth == 0) {
        kprintf("   (no frames available)\n");
    }
}

void kpanic_with_frame(const char *file, int line,
                       const interrupt_frame_t *f, const char *fmt, ...) {
    __asm__ volatile ("cli");

    if (g_panicking) {
        halt_forever();   /* recursive panic: say nothing more */
    }
    g_panicking = true;

    klog_set_min_level(LOG_DEBUG);
    if (fb_console_ready()) {
        fb_console_set_colors(COLOR_WHITE, COLOR_PANIC_BG);
        fb_console_clear();
        klog_enable_fb(true);
    }

    klog_style(LOG_FATAL);
    kprintf("\n=================== KERNEL PANIC ===================");
    klog_style_reset();
    kprintf("\n");

    klog_style(LOG_ERROR);
    kprintf(" Reason : ");
    klog_style_reset();

    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);

    kprintf("\n Where  : %s:%d\n", file, line);

    u64 rbp, rsp;
    u64 rip = 0;

    if (f != NULL) {
        kprintf("\n CPU exception state (from interrupt frame):\n");
        kprintf("   RAX=%016lx RBX=%016lx RCX=%016lx\n", f->rax, f->rbx, f->rcx);
        kprintf("   RDX=%016lx RSI=%016lx RDI=%016lx\n", f->rdx, f->rsi, f->rdi);
        kprintf("   RBP=%016lx RSP=%016lx R8 =%016lx\n", f->rbp, f->rsp, f->r8);
        kprintf("   R9 =%016lx R10=%016lx R11=%016lx\n", f->r9, f->r10, f->r11);
        kprintf("   R12=%016lx R13=%016lx R14=%016lx\n", f->r12, f->r13, f->r14);
        kprintf("   R15=%016lx RIP=%016lx RFLAGS=%016lx\n", f->r15, f->rip, f->rflags);
        kprintf("   CS =%04lx SS =%04lx  vector=%lu error=0x%lx\n",
                f->cs, f->ss, f->vector, f->error_code);
        rbp = f->rbp;
        rsp = f->rsp;
        rip = f->rip;
    } else {
        /* Software panic: GPR contents at this point are meaningless, so we
         * only report the stack/flags state. */
        rbp = (u64)(uintptr_t)__builtin_frame_address(0);
        __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
        kprintf("\n Software panic (no exception frame):\n");
        kprintf("   RSP=%016lx RBP=%016lx RFLAGS=%016lx\n", rsp, rbp, read_rflags());
    }

    kprintf("   CR0=%016lx CR2=%016lx CR3=%016lx\n", read_cr0(), read_cr2(), read_cr3());
    kprintf("   CR4=%016lx EFER=%016lx\n", read_cr4(), rdmsr(MSR_EFER));

    print_backtrace(rbp, rip);

    klog_style(LOG_FATAL);
    kprintf("\n System halted.");
    klog_style_reset();
    kprintf("\n");

    halt_forever();
}