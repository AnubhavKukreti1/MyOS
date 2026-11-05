/*
 * MyOS - src/main.c
 * Boot order: serial -> boot data -> framebuffer -> CPU -> GDT/TSS/IDT ->
 * memory map -> PMM -> VMM (own CR3) -> own stack -> reclaim -> heap ->
 * PIC/PIT/keyboard/syscalls/PCI/AHCI -> scheduler -> user process.
 */

#include "kernel.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "arch/x86_64/idt.h"
#include "drivers/timer.h"
#include "drivers/keyboard.h"
#include "drivers/pci.h"
#include "drivers/ahci.h"
#include "kernel/scheduler.h"
#include "kernel/process.h"
#include "kernel/syscall.h"
#include "kernel/shell.h"

#ifdef MYOS_SELFTEST_INT3
static void selftest_breakpoint(interrupt_frame_t *f) {
    kok("Self-test: #BP handled at RIP=0x%lx - exception path works", f->rip);
}
#endif

#define BOOT_STACK_SIZE (32 * KiB)
static u8 g_boot_stack[BOOT_STACK_SIZE] __attribute__((aligned(16)));

extern const u8 user_demo_start[], user_demo_entry[], user_demo_end[];

NORETURN void kernel_main_late(void);

static void print_banner(void) {
    klog_style(LOG_INFO);
    kputs("\n====================================================\n");
    kputs("   " KERNEL_NAME " " KERNEL_VERSION " - x86_64 - Limine boot protocol\n");
    kputs("====================================================\n");
    klog_style_reset();
}

/* ---- STAGE C/D demo tasks ------------------------------------------------ */
/* Debug-only: the shell is the keyboard's single consumer. A second thread
 * calling keyboard_poll() steals keystrokes from it, and the ping/pong
 * output interleaves with the shell prompt. Build with -DMYOS_DEMO_THREADS
 * to get them back. */
#ifdef MYOS_DEMO_THREADS
static void demo_thread(void *arg) {
    const char *name = arg;
    for (int i = 0; i < 5; i++) {
        kinfo("[%s] iteration %d (tick %lu)", name, i,
              (unsigned long)timer_ticks());
        sched_sleep_ms(300);
    }
}

static void kbd_thread(void *arg) {
    UNUSED(arg);
    kinfo("keyboard: click the QEMU window and type");
    for (;;) {
        key_event_t ev;
        if (keyboard_poll(&ev)) {
            if (ev.pressed && ev.ascii != 0) {
                kputc(ev.ascii);
            }
        } else {
            sched_sleep_ms(10);
        }
    }
}
#endif

/* ---- storage bring-up ------------------------------------------------------ */

static void storage_init(void) {
    // pci_init expects the RSDP physical address (pass 0 to scan BIOS area)
    pci_init(0);
    kok("PCI bus scanned");

    /* Search for class 0x01 (mass storage), subclass 0x06 (SATA), 
     * prog-if 0x01 (AHCI), at index 0 using pci_find_class */
    pci_device_t *ahci_dev = pci_find_class(0x01, 0x06, 0x01, 0);
    if (!ahci_dev) {
        kwarn("No AHCI SATA controller detected on PCI bus");
        return;
    }

    int ports = ahci_init(ahci_dev);
    if (ports > 0) {
        kok("AHCI controller initialized: %d usable SATA port(s)", ports);
    } else {
        kwarn("AHCI controller found but no usable SATA disk (rc=%d)", ports);
    }
}

/* ---- early boot (still on Limine's stack and page tables) ----------------- */

NORETURN void kernel_main(void) {
    bool serial_ok = serial_init();

    boot_collect(&g_boot);
    if (g_boot.checks[BOOT_CHK_FRAMEBUFFER].ok && fb_console_init(&g_boot.fb)) {
        klog_enable_fb(true);
    }

    print_banner();
    if (!serial_ok) {
        kwarn("COM1 self-test failed: serial output disabled");
    } else {
        kok("COM1 serial initialised (38400 8N1)");
    }
    if (fb_console_ready()) {
        kok("Framebuffer console online");
    }

    boot_report_and_enforce(&g_boot);

    cpu_info_t cpu;
    cpu_early_init(&cpu);
    cpu_report(&cpu);

    gdt_init();
    kok("GDT + TSS loaded");
    idt_init();
    kok("IDT loaded: %u vectors", (unsigned)IDT_VECTORS);

#ifdef MYOS_SELFTEST_INT3
    idt_register_handler(3, selftest_breakpoint);
    __asm__ volatile ("int3");
#endif

    mem_info_t mem;
    mem_init(&g_boot, &mem);
    mem_report(&mem);

    pmm_plan_t plan;
    pmm_plan_bitmap(&mem, &plan);
    pmm_report(&mem, &plan);
    if (!plan.found) {
        kpanic("Cannot place the PMM bitmap");
    }
    if (!pmm_init(&mem, &plan)) {
        kpanic("PMM initialisation failed");
    }
    pmm_test();
    pmm_dump_stats();

    vmm_init(&g_boot);                       /* CR3 now points at OUR tables */

    /* Leave Limine's stack: everything above (mem, plan, cpu) is dead after
     * this. Top is 16-byte aligned, so after `call` pushes the return address
     * the callee sees the ABI-mandated rsp % 16 == 8. */
    u64 top = (u64)(uintptr_t)(g_boot_stack + sizeof g_boot_stack);
    __asm__ volatile ("movq %0, %%rsp\n\t"
                      "xorl %%ebp, %%ebp\n\t"
                      "call kernel_main_late"
                      : : "r"(top) : "memory", "cc");
    __builtin_unreachable();
}

/* ---- late boot (own stack, own page tables) ------------------------------- */

NORETURN void kernel_main_late(void) {
    kinfo("Running on the kernel's own stack and page tables");

    boot_reclaim_memory();                   /* STAGE A */
    pmm_dump_stats();
    heap_init();
    heap_test();

    pic_init();                              /* STAGE B */
    if (!timer_init(100)) {
        kpanic("PIT initialisation failed");
    }
    keyboard_init();
    syscall_init();

    storage_init();                          /* PCI + AHCI */

    sched_init();                            /* STAGE C */
#ifdef MYOS_DEMO_THREADS
    sched_spawn("ping", demo_thread, "ping");
    sched_spawn("pong", demo_thread, "pong");
    sched_spawn("kbd",  kbd_thread,  NULL);
#endif
    sched_spawn("shell", shell_thread, NULL);

    process_create_user("hello", user_demo_start,   /* STAGE D */
                        (size_t)(user_demo_end - user_demo_start),
                        (u64)(user_demo_entry - user_demo_start));

    kok("%s %s: entering scheduler, interrupts on", KERNEL_NAME, KERNEL_VERSION);
    sched_idle_loop();

    /* sched_idle_loop() must never return; if it does, don't fall off the end. */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}