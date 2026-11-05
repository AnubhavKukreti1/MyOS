/*
 * MyOS - src/kernel/scheduler.h
 * Preemptive/Cooperative Round-Robin Multitasking Scheduler.
 */

#ifndef MYOS_KERNEL_SCHEDULER_H
#define MYOS_KERNEL_SCHEDULER_H

#include "kernel.h"
#include "mm/vmm.h"

#define KSTACK_SIZE         (16 * KiB)
#define SCHED_QUANTUM_TICKS 5               /* 50 ms quantum at 100 Hz */

/**
 * @brief Thread/Process execution states.
 */
typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_BLOCKED,
    TASK_ZOMBIE
} task_state_t;

/**
 * @brief Process Control Block (PCB) representing an address space and its resources.
 */
typedef struct process {
    u32             pid;
    char            name[16];
    address_space_t *as;
    u32             nthreads;
} process_t;

/**
 * @brief Thread Control Block (TCB) representing an independent execution context.
 */
typedef struct task {
    u32                 tid;
    char                name[16];
    task_state_t        state;
    interrupt_frame_t   *frame;             /* Saved register context, lives on kstack */
    u8                  *kstack;
    u64                 kstack_top;         /* Also TSS.rsp0 while this task runs */
    bool                owns_stack;
    process_t           *proc;              /* NULL for kernel threads */
    u64                 wake_tick;
    u32                 ticks_left;
    int                 exit_code;
    struct task         *next, *prev;       /* Round-robin circular doubly-linked list */
} task_t;

/**
 * @brief Safely copy a string with null-termination guarantee.
 */
static inline void name_copy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i + 1 < n && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/**
 * @brief Initialize the multitasking subsystem; current execution context becomes "idle".
 */
void sched_init(void);

/**
 * @brief Spawn a new kernel thread.
 * @param name Human-readable identifier.
 * @param entry Function pointer to thread entry point.
 * @param arg Argument passed to the entry function.
 * @return Pointer to the created task_t structure, or NULL on failure.
 */
task_t *sched_spawn(const char *name, void (*entry)(void *), void *arg);

/**
 * @brief Spawn a new user-space process/thread.
 * @param name Human-readable identifier.
 * @param p Pointer to the parent process_t.
 * @param entry Virtual address of the user entry point.
 * @param user_sp Initial user-space stack pointer.
 * @return Pointer to the created task_t structure, or NULL on failure.
 */
task_t *sched_spawn_user(const char *name, process_t *p, u64 entry, u64 user_sp);

/**
 * @brief Get the currently executing task.
 * @return Pointer to the active task_t.
 */
task_t *sched_current(void);

/**
 * @brief Voluntarily yield the remaining CPU time quantum.
 */
void sched_yield(void);

/**
 * @brief Put the current task to sleep for a specified duration.
 * @param ms Sleep duration in milliseconds.
 */
void sched_sleep_ms(u32 ms);

/**
 * @brief Terminate the current task.
 * @param code Exit status code.
 */
NORETURN void sched_exit(int code);

/**
 * @brief System idle loop executed when no other tasks are runnable.
 */
NORETURN void sched_idle_loop(void);

/**
 * @brief Periodic scheduler tick handler invoked by the timer subsystem.
 */
void sched_tick(void);

/* Interrupt/Syscall context wrappers: mark intent, switch happens on return */
void sched_isr_yield(void);
void sched_isr_exit(int code);
void sched_isr_sleep(u32 ms);

#endif /* MYOS_KERNEL_SCHEDULER_H */