/**
 * @file spinlock.h
 * @brief Ticket spinlock (FIFO-fair, SMP-ready). On one CPU the *_irqsave
 * variants are what make it safe against interrupt handlers.
 * A lock held for more than ~1 billion pause iterations panics instead of
 * hanging silently.
 */
#ifndef MYOS_KERNEL_SPINLOCK_H
#define MYOS_KERNEL_SPINLOCK_H

#include "kernel.h"

typedef struct {
    volatile u32 next;    /* next ticket to hand out */
    volatile u32 owner;   /* ticket currently being served */
} spinlock_t;

#define SPINLOCK_INIT       { 0, 0 }
#define SPIN_DEADLOCK_LIMIT (1ULL << 30)

static inline void spin_init(spinlock_t *l) {
    l->next  = 0;
    l->owner = 0;
}

static inline void spin_lock(spinlock_t *l) {
    u32 ticket = __atomic_fetch_add(&l->next, 1u, __ATOMIC_RELAXED);
    u64 spins = 0;

    while (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != ticket) {
        __asm__ volatile ("pause" : : : "memory");
        if (++spins > SPIN_DEADLOCK_LIMIT) {
            kpanic("spinlock %p: deadlock suspected (ticket %u, owner %u)",
                   (void *)l, (unsigned)ticket, (unsigned)l->owner);
        }
    }
}

static inline bool spin_trylock(spinlock_t *l) {
    u32 owner    = __atomic_load_n(&l->owner, __ATOMIC_RELAXED);
    u32 expected = owner;

    return __atomic_compare_exchange_n(&l->next, &expected, owner + 1u, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline void spin_unlock(spinlock_t *l) {
    __atomic_store_n(&l->owner, l->owner + 1u, __ATOMIC_RELEASE);
}

static inline irq_flags_t spin_lock_irqsave(spinlock_t *l) {
    irq_flags_t f = irq_save();
    spin_lock(l);
    return f;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, irq_flags_t f) {
    spin_unlock(l);
    irq_restore(f);
}

#endif