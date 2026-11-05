/*
 * MyOS - src/drivers/timer.c
 * 8253/8254 Programmable Interval Timer (PIT), channel 0, mode 2 (rate generator).
 */

#include "kernel.h"
#include "arch/x86_64/idt.h"
#include "drivers/timer.h"

#define PIT_CH0     0x40
#define PIT_CMD     0x43
#define PIT_BASE_HZ 1193182u

static volatile u64 g_ticks;
static u32          g_hz;
static void       (*g_tick_hook)(void);

static void timer_irq(interrupt_frame_t *f) {
    UNUSED(f);
    g_ticks++;
    if (g_tick_hook != NULL) {
        g_tick_hook();
    }
}

bool timer_init(u32 hz) {
    if (hz == 0) {
        return false;
    }
    u32 div = (PIT_BASE_HZ + hz / 2) / hz;
    if (div == 0) {
        div = 1;
    }
    if (div > 0xFFFF) {
        return false;                           /* below ~19 Hz limit */
    }
    g_hz = (PIT_BASE_HZ + div / 2) / div;

    outb(PIT_CMD, 0x34);                        /* ch0, lo/hi byte, mode 2, binary */
    outb(PIT_CH0, (u8)(div & 0xFF));
    outb(PIT_CH0, (u8)(div >> 8));

    irq_register(0, timer_irq);
    kok("PIT: %u Hz (divisor %u)", g_hz, div);
    return true;
}

u64  timer_ticks(void) { return g_ticks; }
u32  timer_hz(void)    { return g_hz; }
u64  timer_ms(void)    { return g_hz ? g_ticks * 1000 / g_hz : 0; }
void timer_set_tick_hook(void (*hook)(void)) { g_tick_hook = hook; }

void timer_wait_ms(u32 ms) {
    u64 target = g_ticks + ((u64)ms * g_hz + 999) / 1000;
    while (g_ticks < target) {
        __asm__ volatile ("sti; hlt" : : : "memory");
    }
}