/*
 * MyOS - include/mm/pmm.h
 * Physical Memory Manager: bitmap frame allocator (1 bit = 1 x 4 KiB frame,
 * 0 = free, 1 = used/reserved).
 */

#ifndef MYOS_MM_PMM_H
#define MYOS_MM_PMM_H

#include "kernel.h"

/* =========================================================================
   TYPES & CONSTANTS
   ========================================================================= */

/* A physical address type. */
typedef u64 paddr_t;

/* Returned on out-of-memory. Safe as a sentinel: frame 0 is never handed out. */
#define PMM_ALLOC_FAIL          ((paddr_t)0)

/* Everything below this is never allocated (null page, DMA, AP trampoline). */
#define PMM_LOW_RESERVE         (1 * MiB)

/* ---- Bitmap index helpers ------------------------------------------------ */
#define PMM_BITS_PER_WORD       64u
#define PMM_ADDR_TO_FRAME(a)    ((u64)(a) / PAGE_SIZE)
#define PMM_FRAME_TO_ADDR(f)    ((u64)(f) * PAGE_SIZE)
#define PMM_FRAMES_TO_BYTES(n)  ((u64)(n) * PAGE_SIZE)
#define PMM_WORD_OF(f)          ((u64)(f) / PMM_BITS_PER_WORD)
#define PMM_BIT_OF(f)           ((u64)(f) % PMM_BITS_PER_WORD)

/* Deallocation / validation error codes. */
typedef enum {
    PMM_OK = 0,
    PMM_ERR_NOT_INIT,
    PMM_ERR_UNALIGNED,      /* Address not 4 KiB page-aligned             */
    PMM_ERR_OUT_OF_RANGE,   /* Beyond the highest frame the bitmap covers   */
    PMM_ERR_RESERVED,       /* Kernel, MMIO, bitmap, low memory, etc.     */
    PMM_ERR_DOUBLE_FREE     /* Frame is already marked as free            */
} pmm_status_t;

/* =========================================================================
   PUBLIC API
   ========================================================================= */

/**
 * @brief Initialize the Physical Memory Manager bitmap allocator.
 * 
 * @param mi   Pointer to the system memory map info structure.
 * @param plan Pointer to the validated PMM allocation plan.
 * @return true on success, false on initialization failure.
 */
bool pmm_init(const mem_info_t *mi, const pmm_plan_t *plan);

/* ---- Allocation --------------------------------------------------------- */

/**
 * @brief Allocate a single physical frame (4 KiB).
 * @return Physical address of the frame, or PMM_ALLOC_FAIL on exhaustion.
 */
paddr_t pmm_alloc_frame(void);

/**
 * @brief Allocate a single physical frame and zero-initialize its contents via HHDM.
 * @return Physical address of the zeroed frame, or PMM_ALLOC_FAIL on exhaustion.
 */
paddr_t pmm_alloc_frame_zeroed(void);

/**
 * @brief Allocate a single physical frame or panic the kernel if out of memory.
 * @return Physical address of the allocated frame.
 */
paddr_t pmm_alloc_frame_or_panic(void);

/* ---- Deallocation & Reclamation ----------------------------------------- */

/**
 * @brief Attempt to free a physical frame, returning a detailed status code.
 * @param addr Physical address of the frame to free (must be page-aligned).
 */
pmm_status_t pmm_try_free(paddr_t addr);

/**
 * @brief Free a physical frame, logging a warning if the operation is refused.
 * @param addr Physical address of the frame to free.
 * @return true if successfully freed, false otherwise.
 */
bool pmm_free_frame(paddr_t addr);

/**
 * @brief Reclaim a physical memory range (e.g., bootloader reclaimable sections)
 *        and add its frames to the PMM management pool.
 * 
 * @param base   Physical base address of the range.
 * @param length Length of the range in bytes.
 * @return u64   Number of physical frames successfully released/reclaimed.
 */
u64 pmm_reclaim_range(u64 base, u64 length);

/**
 * @brief Permanently keep [base, base + length) out of circulation (rounded outward).
 * @param base   Physical base address.
 * @param length Length in bytes.
 */
bool pmm_reserve_range(u64 base, u64 length);

/* ---- Introspection & Diagnostics ---------------------------------------- */

/**
 * @brief Check whether a specific physical frame address is currently used/reserved.
 */
bool pmm_is_frame_used(paddr_t addr);

/* Frame count queries */
u64  pmm_get_total_frames(void);
u64  pmm_get_free_frames(void);
u64  pmm_get_used_frames(void);

/* Byte amount queries */
u64  pmm_get_total_memory_amount(void);   /* Managed RAM in bytes */
u64  pmm_get_free_memory_amount(void);    /* Free RAM in bytes */
u64  pmm_get_used_memory_amount(void);    /* Used RAM in bytes */

/**
 * @brief Output current memory statistics via the kernel logging system.
 */
void pmm_dump_stats(void);

/**
 * @brief Convert a PMM status error code to a readable string description.
 */
const char *pmm_status_name(pmm_status_t st);

/**
 * @brief Run comprehensive self-tests on the PMM (panics via KASSERT on error).
 */
void pmm_test(void);

/**
 * @brief Allocate `count` physically contiguous frames.
 * @param count    number of 4 KiB frames (> 0)
 * @param max_end  exclusive upper physical bound (0 = unlimited)
 * @param align    byte alignment of the first frame (power of two, >= PAGE_SIZE)
 * @return physical address or PMM_ALLOC_FAIL
 */
paddr_t pmm_alloc_contiguous(u64 count, u64 max_end, u64 align);

/** @brief Free `count` frames starting at `addr`. Returns frames actually freed. */
u64 pmm_free_contiguous(paddr_t addr, u64 count);

#endif /* MYOS_MM_PMM_H */