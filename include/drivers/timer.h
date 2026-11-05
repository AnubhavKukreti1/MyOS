/*
 * MyOS - src/drivers/timer.h
 * Programmable Interval Timer (PIT) and system time tracking.
 */

#ifndef MYOS_DRIVERS_TIMER_H
#define MYOS_DRIVERS_TIMER_H

#include "kernel.h"

/* PIT oscillator base frequency in Hz (~1.193182 MHz) */
#define PIT_BASE_FREQUENCY 1193182UL

/* Valid programmable frequency bounds for PIT channel 0 */
#define TIMER_MIN_HZ       19UL
#define TIMER_MAX_HZ       PIT_BASE_FREQUENCY

/**
 * @brief Initialize the system timer (PIT channel 0).
 * @param hz Desired frequency in Hz (must be within TIMER_MIN_HZ..TIMER_MAX_HZ).
 * @return true if successfully initialized, false if hz is out of bounds.
 */
bool timer_init(u32 hz);

/**
 * @brief Get the total number of timer interrupts (ticks) elapsed since boot.
 * @return 64-bit tick count.
 */
u64 timer_ticks(void);

/**
 * @brief Get the actual programmed tick frequency in Hz.
 * @return Frequency value actually applied to the hardware.
 */
u32 timer_hz(void);

/**
 * @brief Get the elapsed system uptime in milliseconds.
 * @return Milliseconds since boot.
 */
u64 timer_ms(void);

/**
 * @brief Suspend execution for a specified duration using CPU halt instructions.
 * @param ms Duration to wait in milliseconds.
 * @note Suitable for early boot or diagnostic code before the scheduler is active.
 */
void timer_wait_ms(u32 ms);

/**
 * @brief Register an optional callback hook executed on every timer tick interrupt.
 * @param hook Function pointer to the tick handler (pass NULL to clear).
 */
void timer_set_tick_hook(void (*hook)(void));

#endif /* MYOS_DRIVERS_TIMER_H */