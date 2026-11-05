/**
 * @file timeout.h
 * @brief Bounded-wait helpers usable both before and after interrupts are on.
 *
 * With interrupts enabled and the PIT running, deadlines use timer ticks.
 * Otherwise (early boot, inside irq-disabled sections) they fall back to an
 * iteration budget of ~1000 loops per millisecond. That assumes every polling
 * iteration costs about a microsecond: callers do an MMIO read or kio_delay()
 * per iteration. A large iteration backstop also protects tick mode against
 * interrupts being disabled mid-wait.
 */
#ifndef MYOS_KERNEL_TIMEOUT_H
#define MYOS_KERNEL_TIMEOUT_H

#include "kernel.h"
#include "drivers/timer.h"

typedef struct {
    u64  deadline_tick;
    u64  budget;
    bool tick_mode;
} kdeadline_t;

/** @brief ~1 microsecond delay on real hardware (ISA bus write to port 0x80). */
static inline void kio_delay(void) {
    outb(0x80, 0);
}

static inline bool kirq_enabled(void) {
    return (read_rflags() & (1ULL << 9)) != 0;
}

static inline void kdeadline_start(kdeadline_t *d, u32 ms) {
    d->tick_mode = (timer_hz() != 0) && kirq_enabled();
    if (d->tick_mode) {
        d->deadline_tick = timer_ticks() + ((u64)ms * timer_hz() + 999u) / 1000u + 1u;
        d->budget        = (u64)ms * 20000u + 1000u;
    } else {
        d->deadline_tick = 0;
        d->budget        = (u64)ms * 1000u + 16u;
    }
}

/** @brief Call once per polling iteration. True once the wait must be abandoned. */
static inline bool kdeadline_expired(kdeadline_t *d) {
    if (d->budget == 0) {
        return true;
    }
    d->budget--;
    return d->tick_mode && timer_ticks() >= d->deadline_tick;
}

/** @brief Busy-wait for roughly `ms` milliseconds. */
static inline void kdelay_ms(u32 ms) {
    if (timer_hz() != 0 && kirq_enabled()) {
        kdeadline_t d;
        kdeadline_start(&d, ms);
        while (!kdeadline_expired(&d)) {
            cpu_relax();
        }
    } else {
        for (u64 i = 0; i < (u64)ms * 1000u; i++) {
            kio_delay();
        }
    }
}

#endif