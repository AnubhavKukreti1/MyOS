/**
 * @file dma.h
 * @brief DMA buffer allocation, address translation and coherency helpers.
 *
 * Coherency model: x86 DMA to write-back memory is cache-coherent, so
 * ordering barriers are all that is normally required. dma_set_coherent(false)
 * switches the sync helpers to explicit CLFLUSH for exotic platforms.
 */
#ifndef MYOS_MM_DMA_H
#define MYOS_MM_DMA_H

#include "kernel.h"

#define DMA_MASK_32   0x00000000FFFFFFFFULL
#define DMA_MASK_64   0xFFFFFFFFFFFFFFFFULL
#define DMA_ZERO      (1u << 0)            /* zero the buffer */
#define DMA_MAX_ALLOC (16ULL * MiB)

typedef struct dma_buf {
    void  *virt;       /* HHDM virtual address */
    u64    phys;       /* physical (bus) address */
    size_t size;       /* requested size in bytes */
    u64    pages;      /* frames actually reserved */
    u64    mask;       /* address mask it was allocated under */
    u32    magic;
} dma_buf_t;

typedef enum {
    DMA_TO_DEVICE      = 1,
    DMA_FROM_DEVICE    = 2,
    DMA_BIDIRECTIONAL  = 3
} dma_dir_t;

typedef struct {
    u64 allocs, frees, failures, bytes_in_use;
} dma_stats_t;

/* ---- Barriers ---- */
static inline void dma_mb(void)  { __asm__ volatile ("mfence" : : : "memory"); }
static inline void dma_wmb(void) { __asm__ volatile ("sfence" : : : "memory"); }
static inline void dma_rmb(void) { __asm__ volatile ("lfence" : : : "memory"); }

/** @brief Probe CLFLUSH support / line size. Call once at boot. */
void dma_init(void);

/** @brief Declare whether device DMA snoops the CPU caches (default true). */
void dma_set_coherent(bool coherent);

/**
 * @brief Allocate physically contiguous, page-aligned DMA memory.
 * @param buf   receives the descriptor
 * @param size  bytes (rounded up to pages); 1 .. DMA_MAX_ALLOC
 * @param align byte alignment, power of two (0 = page)
 * @param mask  highest address the device can drive: DMA_MASK_32 / DMA_MASK_64
 *              or any 2^n - 1
 * @param flags DMA_ZERO
 * @return 0, -E_INVAL, or -E_NOMEM
 */
int32_t dma_alloc(dma_buf_t *buf, size_t size, size_t align, u64 mask, u32 flags);

/** @brief Release a buffer. Detects double free via the magic field. */
void dma_free(dma_buf_t *buf);

/**
 * @brief Translate a kernel virtual address (heap, HHDM, kernel image, MMIO
 *        window) to its physical address by walking the page tables.
 * @return 0, -E_INVAL (NULL or not a kernel address), -E_FAULT (unmapped)
 */
int32_t dma_virt_to_phys(const void *virt, u64 *phys);

/** @brief Physical -> kernel virtual via the HHDM (RAM only). */
static inline void *dma_phys_to_virt(u64 phys) { return phys_to_virt(phys); }

/** @brief True if [phys, phys+len) is entirely addressable under `mask`. */
bool dma_addr_ok(u64 phys, size_t len, u64 mask);

/**
 * @brief Translate a physically contiguous kernel buffer for a device.
 * @return 0, -E_INVAL (not physically contiguous: use scatter-gather),
 *         -E_ADDR (beyond the mask), -E_FAULT
 */
int32_t dma_map_single(const void *virt, size_t len, dma_dir_t dir, u64 mask, u64 *phys);
void    dma_unmap_single(const void *virt, size_t len, dma_dir_t dir);

/** @brief Make CPU writes visible to the device / device writes visible to the CPU. */
void dma_sync_for_device(const void *virt, size_t len, dma_dir_t dir);
void dma_sync_for_cpu(const void *virt, size_t len, dma_dir_t dir);

/** @brief Write back + invalidate a range with CLFLUSH (WBINVD without it). */
void dma_flush_range(const void *virt, size_t len);

void dma_get_stats(dma_stats_t *out);

#endif