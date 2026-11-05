/*
 * MyOS - src/mm/dma.c
 * Contiguous DMA allocator on top of the PMM bitmap.
 * Buffers are accessed through the HHDM (write-back), which is correct for
 * coherent x86 DMA. Counters are atomic, so no lock is needed here; the PMM
 * serialises the actual frame allocation.
 */

#include "kernel.h"
#include "kernel/error.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/dma.h"

#define DMA_MAGIC     0xD3A0B0F5u
#define DMA_MAX_ALIGN (1ULL * MiB)

static struct {
    volatile u64 allocs, frees, failures, bytes_in_use;
    bool         coherent;
    bool         has_clflush;
    u64          line;
} g_dma = { .coherent = true, .has_clflush = true, .line = 64 };

void dma_init(void) {
    u32 a, b, c, d;

    cpuid(1, 0, &a, &b, &c, &d);
    g_dma.has_clflush = ((d >> 19) & 1u) != 0;
    u32 line = ((b >> 8) & 0xFFu) * 8u;
    g_dma.line = (line != 0) ? line : 64u;
    kinfo("DMA: CLFLUSH %s, cache line %lu bytes",
          g_dma.has_clflush ? "yes" : "no", g_dma.line);
}

void dma_set_coherent(bool coherent) {
    g_dma.coherent = coherent;
}

int32_t dma_alloc(dma_buf_t *buf, size_t size, size_t align, u64 mask, u32 flags) {
    if (buf == NULL) {
        return -E_INVAL;
    }
    memset(buf, 0, sizeof *buf);

    if (size == 0 || size > DMA_MAX_ALLOC) {
        return -E_INVAL;
    }
    if (align < PAGE_SIZE) {
        align = PAGE_SIZE;
    }
    if ((align & (align - 1)) != 0 || align > DMA_MAX_ALIGN) {
        return -E_INVAL;
    }
    if ((mask & (mask + 1)) != 0) {              /* must be 2^n - 1 */
        return -E_INVAL;
    }

    const u64 pages   = ALIGN_UP((u64)size, PAGE_SIZE) / PAGE_SIZE;
    const u64 max_end = (mask == DMA_MASK_64) ? 0 : mask + 1;
    paddr_t   p       = pmm_alloc_contiguous(pages, max_end, align);

    if (p == PMM_ALLOC_FAIL) {
        __atomic_fetch_add(&g_dma.failures, 1, __ATOMIC_RELAXED);
        return -E_NOMEM;
    }

    buf->phys  = p;
    buf->virt  = phys_to_virt(p);
    buf->size  = size;
    buf->pages = pages;
    buf->mask  = mask;
    buf->magic = DMA_MAGIC;

    if (flags & DMA_ZERO) {
        memset(buf->virt, 0, pages * PAGE_SIZE);
    }
    dma_wmb();

    __atomic_fetch_add(&g_dma.allocs, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_dma.bytes_in_use, pages * PAGE_SIZE, __ATOMIC_RELAXED);
    return 0;
}

void dma_free(dma_buf_t *buf) {
    if (buf == NULL) {
        return;
    }
    if (buf->magic != DMA_MAGIC) {
        kwarn("dma_free: invalid or already-freed buffer %p", (void *)buf);
        return;
    }

    u64 freed = pmm_free_contiguous(buf->phys, buf->pages);
    if (freed != buf->pages) {
        kerror("dma_free: only %lu of %lu frames returned for phys 0x%lx",
               freed, buf->pages, buf->phys);
    }
    __atomic_fetch_add(&g_dma.frees, 1, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&g_dma.bytes_in_use, buf->pages * PAGE_SIZE, __ATOMIC_RELAXED);
    memset(buf, 0, sizeof *buf);
}

int32_t dma_virt_to_phys(const void *virt, u64 *phys) {
    if (virt == NULL || phys == NULL) {
        return -E_INVAL;
    }
    u64 va = (u64)(uintptr_t)virt;
    if (va < HIGHER_HALF_START) {                /* user pointers are never DMA-able */
        return -E_INVAL;
    }
    u64 pa;
    if (!vmm_translate_in(vmm_kernel_space(), va, &pa, NULL)) {
        return -E_FAULT;
    }
    *phys = pa;
    return 0;
}

bool dma_addr_ok(u64 phys, size_t len, u64 mask) {
    if (len == 0) {
        return false;
    }
    u64 last = phys + (u64)len - 1;
    return last >= phys && last <= mask;
}

int32_t dma_map_single(const void *virt, size_t len, dma_dir_t dir, u64 mask, u64 *phys) {
    if (virt == NULL || phys == NULL || len == 0) {
        return -E_INVAL;
    }
    const u64 va = (u64)(uintptr_t)virt;
    u64 first;
    int32_t rc = dma_virt_to_phys(virt, &first);

    if (rc != 0) {
        return rc;
    }

    /* Every page after the first must follow it physically. */
    u64 next_va = ALIGN_DOWN(va, PAGE_SIZE) + PAGE_SIZE;
    u64 expect  = ALIGN_DOWN(first, PAGE_SIZE) + PAGE_SIZE;
    for (; next_va < va + len; next_va += PAGE_SIZE, expect += PAGE_SIZE) {
        u64 pa;
        rc = dma_virt_to_phys((const void *)(uintptr_t)next_va, &pa);
        if (rc != 0) {
            return rc;
        }
        if (pa != expect) {
            return -E_INVAL;
        }
    }
    if (!dma_addr_ok(first, len, mask)) {
        return -E_ADDR;
    }
    dma_sync_for_device(virt, len, dir);
    *phys = first;
    return 0;
}

void dma_unmap_single(const void *virt, size_t len, dma_dir_t dir) {
    dma_sync_for_cpu(virt, len, dir);
}

void dma_flush_range(const void *virt, size_t len) {
    if (virt == NULL || len == 0) {
        return;
    }
    if (!g_dma.has_clflush) {
        __asm__ volatile ("wbinvd" : : : "memory");
        return;
    }
    const uintptr_t line = (uintptr_t)g_dma.line;
    uintptr_t p   = (uintptr_t)virt & ~(line - 1);
    uintptr_t end = (uintptr_t)virt + len;

    dma_mb();
    for (; p < end; p += line) {
        __asm__ volatile ("clflush (%0)" : : "r"(p) : "memory");
    }
    dma_mb();
}

void dma_sync_for_device(const void *virt, size_t len, dma_dir_t dir) {
    UNUSED(dir);
    if (!g_dma.coherent) {
        dma_flush_range(virt, len);
    }
    dma_mb();
}

void dma_sync_for_cpu(const void *virt, size_t len, dma_dir_t dir) {
    dma_mb();
    if (!g_dma.coherent && (dir & DMA_FROM_DEVICE)) {
        dma_flush_range(virt, len);               /* CLFLUSH also invalidates */
    }
    dma_rmb();
}

void dma_get_stats(dma_stats_t *out) {
    if (out == NULL) {
        return;
    }
    out->allocs       = g_dma.allocs;
    out->frees        = g_dma.frees;
    out->failures     = g_dma.failures;
    out->bytes_in_use = g_dma.bytes_in_use;
}