/*
 * MyOS - src/kernel/scheduler.c
 * Preemptive round-robin. All state is touched with interrupts off (either
 * inside an ISR or under irq_save()), which is sufficient on one CPU.
 */

#include "kernel.h"
#include "arch/x86_64/idt.h"
#include "drivers/timer.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "kernel/scheduler.h"
#include "kernel/process.h"

static struct {
    bool    active, need_resched;
    task_t *current, *idle, *rr;          /* rr = last ring task that ran */
    u32     nring, next_tid;
} sched;

/* ---- ring --------------------------------------------------------------- */

static void ring_add(task_t *t) {
    if (sched.rr == NULL) {
        t->next = t->prev = t;
        sched.rr = t;
    } else {
        task_t *a = sched.rr;
        t->next = a->next;
        t->prev = a;
        a->next->prev = t;
        a->next = t;
    }
    sched.nring++;
}

static void ring_remove(task_t *t) {
    if (t->next == t) {
        sched.rr = NULL;
    } else {
        t->prev->next = t->next;
        t->next->prev = t->prev;
        if (sched.rr == t) {
            sched.rr = t->prev;
        }
    }
    sched.nring--;
}

static task_t *pick_next(void) {
    task_t *start = sched.rr;
    if (start == NULL) {
        return sched.idle;
    }
    task_t *t = start;
    do {
        t = t->next;
        if (t->state == TASK_READY) {
            return t;
        }
    } while (t != start);
    return sched.idle;
}

/* ---- teardown ------------------------------------------------------------ */

static void task_destroy(task_t *t) {
    if (t->proc != NULL && --t->proc->nthreads == 0) {
        process_destroy(t->proc);
    }
    if (t->owns_stack) {
        kfree(t->kstack);
    }
    kfree(t);
}

/* Never frees `current`: we may still be standing on its stack. */
static void reap_zombies(void) {
    task_t *t = sched.rr;
    u32 n = sched.nring;

    while (n-- != 0 && t != NULL) {
        task_t *nx = t->next;
        if (t->state == TASK_ZOMBIE && t != sched.current) {
            ring_remove(t);
            task_destroy(t);
        }
        t = nx;
    }
}

/* ---- the switch ---------------------------------------------------------- */

static interrupt_frame_t *schedule(interrupt_frame_t *f) {
    task_t *prev = sched.current;

    prev->frame = f;
    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
    }
    reap_zombies();

    task_t *next = pick_next();
    next->state      = TASK_RUNNING;
    next->ticks_left = SCHED_QUANTUM_TICKS;
    if (next != sched.idle) {
        sched.rr = next;
    }

    if (next != prev) {
        sched.current = next;
        tss_set_rsp0(next->kstack_top);
        address_space_t *as = (next->proc != NULL) ? next->proc->as : vmm_kernel_space();
        if ((read_cr3() & PTE_ADDR_MASK) != as->pml4_phys) {
            vmm_switch(as);
        }
    }
    return next->frame;
}

static interrupt_frame_t *sched_dispatch(interrupt_frame_t *f) {
    if (!sched.active || !sched.need_resched) {
        return f;
    }
    sched.need_resched = false;
    return schedule(f);
}

void sched_tick(void) {                   /* timer IRQ context */
    if (!sched.active) {
        return;
    }
    u64 now = timer_ticks();
    task_t *t = sched.rr;
    for (u32 i = 0; i < sched.nring; i++, t = t->next) {
        if (t->state == TASK_SLEEPING && now >= t->wake_tick) {
            t->state = TASK_READY;
        }
    }
    if (sched.current == sched.idle) {
        sched.need_resched = true;        /* cheap: pick_next returns idle if nothing is ready */
    } else if (sched.current->ticks_left != 0 && --sched.current->ticks_left == 0) {
        sched.need_resched = true;
    }
}

/* ---- ISR-context requests ------------------------------------------------ */

void sched_isr_yield(void) { sched.need_resched = true; }

void sched_isr_exit(int code) {
    sched.current->exit_code = code;
    sched.current->state     = TASK_ZOMBIE;
    sched.need_resched       = true;
}

void sched_isr_sleep(u32 ms) {
    u64 ticks = ((u64)ms * timer_hz() + 999) / 1000;
    sched.current->wake_tick = timer_ticks() + (ticks != 0 ? ticks : 1);
    sched.current->state     = TASK_SLEEPING;
    sched.need_resched       = true;
}

static void yield_handler(interrupt_frame_t *f) {
    UNUSED(f);
    sched.need_resched = true;
}

static void user_fault(interrupt_frame_t *f) {
    task_t *t = sched.current;

    kerror("'%s' (tid %u) killed by exception #%lu at RIP=0x%lx, error=0x%lx",
           t->name, (unsigned)t->tid, f->vector, f->rip, f->error_code);
    if (f->vector == 14) {
        kerror("  faulting address CR2=0x%lx", read_cr2());
    }
    sched_isr_exit(-1);
}

/* ---- thread-context API ---------------------------------------------------- */

void sched_yield(void) {
    __asm__ volatile ("int $0x81" : : : "memory");    /* SCHED_YIELD_VECTOR */
}

void sched_sleep_ms(u32 ms) {
    irq_flags_t fl = irq_save();          /* mark + yield must be atomic */
    sched_isr_sleep(ms);
    sched_yield();
    irq_restore(fl);
}

NORETURN void sched_exit(int code) {
    (void)irq_save();                     /* we are dying: never re-enable */
    sched.current->exit_code = code;
    sched.current->state     = TASK_ZOMBIE;
    for (;;) {
        sched_yield();
    }
}

task_t *sched_current(void) { return sched.current; }

NORETURN void sched_idle_loop(void) {
    for (;;) {
        __asm__ volatile ("sti; hlt" : : : "memory");
    }
}

/* ---- creation -------------------------------------------------------------- */

static NORETURN void thread_return(void) {
    sched_exit(0);                        /* a thread function returned */
}

static task_t *task_alloc(const char *name) {
    task_t *t = kzalloc(sizeof *t);
    if (t == NULL) {
        return NULL;
    }
    t->kstack = kmalloc(KSTACK_SIZE);
    if (t->kstack == NULL) {
        kfree(t);
        return NULL;
    }
    t->owns_stack = true;
    t->kstack_top = (u64)(uintptr_t)t->kstack + KSTACK_SIZE;
    name_copy(t->name, name, sizeof t->name);

    irq_flags_t fl = irq_save();
    t->tid = ++sched.next_tid;
    irq_restore(fl);
    return t;
}

/* A first-time context is an interrupt frame that iretq will "return" into. */
static interrupt_frame_t *build_frame(u64 top, u64 rip, u64 cs, u64 ss, u64 rsp, u64 arg) {
    interrupt_frame_t *f = (interrupt_frame_t *)(uintptr_t)(top - sizeof *f);

    memset(f, 0, sizeof *f);
    f->rip    = rip;
    f->cs     = cs;
    f->rflags = 0x202;                    /* IF=1 + reserved bit 1 */
    f->rsp    = rsp;
    f->ss     = ss;
    f->rdi    = arg;                      /* first C argument */
    return f;
}

static void task_start(task_t *t) {
    irq_flags_t fl = irq_save();
    t->state = TASK_READY;
    ring_add(t);
    irq_restore(fl);
}

task_t *sched_spawn(const char *name, void (*entry)(void *), void *arg) {
    task_t *t = task_alloc(name);
    if (t == NULL) {
        return NULL;
    }
    /* rsp must be 8 mod 16 at function entry: pretend a call pushed thread_return. */
    u64 *sp = (u64 *)(uintptr_t)t->kstack_top;
    *--sp = (u64)(uintptr_t)thread_return;
    u64 rsp = (u64)(uintptr_t)sp;

    t->frame = build_frame(rsp, (u64)(uintptr_t)entry, GDT_KERNEL_CS, GDT_KERNEL_DS,
                           rsp, (u64)(uintptr_t)arg);
    task_start(t);
    return t;
}

task_t *sched_spawn_user(const char *name, process_t *p, u64 entry, u64 user_sp) {
    task_t *t = task_alloc(name);
    if (t == NULL) {
        return NULL;
    }
    t->proc  = p;
    t->frame = build_frame(t->kstack_top, entry, GDT_USER_CS | 3, GDT_USER_DS | 3, user_sp, 0);

    irq_flags_t fl = irq_save();
    p->nthreads++;
    irq_restore(fl);
    task_start(t);
    return t;
}

void sched_init(void) {
    task_t *idle = kzalloc(sizeof *idle);
    KASSERT(idle != NULL);

    name_copy(idle->name, "idle", sizeof idle->name);
    idle->state = TASK_RUNNING;
    sched.idle = sched.current = idle;

    idt_register_handler(SCHED_YIELD_VECTOR, yield_handler);
    idt_set_resched_hook(sched_dispatch);
    idt_set_user_fault_handler(user_fault);
    timer_set_tick_hook(sched_tick);
    sched.active = true;
    kok("Scheduler online: round-robin, %u ms quantum",
        (unsigned)(SCHED_QUANTUM_TICKS * 1000 / timer_hz()));
}