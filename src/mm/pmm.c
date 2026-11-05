/*
- MyOS - src/mm/pmm.c
 *
- Bitmap physical frame allocator.
 *
- Design:
- Bitmap lives in physical memory chosen by pmm_plan_bitmap(), accessed
-     through the HHDM. Bit = 1 means used/reserved, 0 means free.
- Init starts all-ones and only clears frames inside MEMT_USABLE regions,
-     so kernel image, bootloader-reclaimable memory, ACPI, framebuffer and
-     MMIO are reserved by construction.
- On top of that we reserve the first 1 MiB and the bitmap's own frames.
- Reserved ranges are remembered so pmm_try_free() can refuse them (a
-     reserved frame and an allocated frame look identical in the bitmap).
- Bits past span_frames (bitmap is page-aligned) stay 1, so the allocator
-     can never return a frame beyond the covered range.
- Single CPU for now: add a spinlock around alloc/free when SMP arrives.
 */

#include "kernel.h"
#include "mm/pmm.h"

/* =========================================================================
   PLANNING (unchanged)
   ========================================================================= */

bool pmm_plan_bitmap(const mem_info_t *mi, pmm_plan_t *plan) {
    memset(plan, 0, sizeof *plan);

    plan->span_frames  = mi->highest_usable_end / PAGE_SIZE;
    plan->bitmap_bytes = ALIGN_UP((plan->span_frames + 7) / 8, PAGE_SIZE);

    /* First fit above 1 MiB: keeps scarce low memory free for DMA/trampolines. */
    for (size_t i = 0; i < mi->count; i++) {
        const mem_region_t *r = &mi->regions[i];
        if (r->type != MEMT_USABLE) {
            continue;
        }

        u64 base = ALIGN_UP(r->base, PAGE_SIZE);
        u64 end  = ALIGN_DOWN(r->base + r->length, PAGE_SIZE);
        if (base < 1 * MiB) {
            base = 1 * MiB;
        }
        if (end > base && end - base >= plan->bitmap_bytes) {
            plan->found        = true;
            plan->bitmap_phys  = base;
            plan->region_index = i;
            return true;
        }
    }
    return false;
}

void pmm_report(const mem_info_t *mi, const pmm_plan_t *plan) {
    char size[32];

    UNUSED(mi);
    fmt_size(size, sizeof size, plan->bitmap_bytes);

    kinfo("PMM plan: bitmap covers %lu frames, needs %s", plan->span_frames, size);

    if (!plan->found) {
        kerror("PMM plan: no usable region large enough for the bitmap");
        return;
    }

    kok("PMM plan: place bitmap at phys 0x%lx (region #%zu), HHDM virt %p",
        plan->bitmap_phys, plan->region_index, phys_to_virt(plan->bitmap_phys));
}

/* =========================================================================
   ALLOCATOR STATE
   ========================================================================= */

#define PMM_MAX_RESERVED  16
#define PMM_MAX_RECLAIMED 16

typedef struct {
    u64 start;   /* page aligned, inclusive */
    u64 end;     /* page aligned, exclusive */
} pmm_range_t;

static struct {
    bool                ready;
    u64                *bitmap;
    u64                 words;          /* bitmap size in 64-bit words        */
    u64                 span_frames;    /* frames covered (0..highest usable) */
    u64                 bitmap_phys;
    u64                 bitmap_bytes;
    u64                 total_frames;   /* usable frames at init              */
    u64                 free_frames;
    u64                 hint_word;      /* every word below this is full      */
    const mem_region_t *regions;        /* points into g_boot (static)        */
    size_t              region_count;
    pmm_range_t         reserved[PMM_MAX_RESERVED];
    size_t              reserved_count;
    pmm_range_t         reclaimed[PMM_MAX_RECLAIMED];
    size_t              reclaimed_count;
} pmm;

/* =========================================================================
   BITMAP PRIMITIVES (no bounds checks: callers guarantee frame < span)
   ========================================================================= */

static bool frame_is_used(u64 frame) {
    return (pmm.bitmap[PMM_WORD_OF(frame)] >> PMM_BIT_OF(frame)) & 1u;
}

/* Returns true if the frame changed state (was free). */
static bool frame_mark_used(u64 frame) {
    u64 *w = &pmm.bitmap[PMM_WORD_OF(frame)];
    u64  m = 1ULL << PMM_BIT_OF(frame);

    if (*w & m) {
        return false;
    }
    *w |= m;
    pmm.free_frames--;
    return true;
}

/* Returns true if the frame changed state (was used). */
static bool frame_mark_free(u64 frame) {
    u64 *w = &pmm.bitmap[PMM_WORD_OF(frame)];
    u64  m = 1ULL << PMM_BIT_OF(frame);

    if (!(*w & m)) {
        return false;
    }
    *w &= ~m;
    pmm.free_frames++;
    if (PMM_WORD_OF(frame) < pmm.hint_word) {
        pmm.hint_word = PMM_WORD_OF(frame);
    }
    return true;
}

/* =========================================================================
   RESERVATIONS
   ========================================================================= */

static bool reserve_range_internal(u64 base, u64 length) {
    if (length == 0) {
        return true;
    }
    u64 end = base + length;
    if (end < base || end > UINT64_MAX - PAGE_SIZE) {
        return false;
    }
    if (pmm.reserved_count >= PMM_MAX_RESERVED) {
        return false;
    }

    u64 start = ALIGN_DOWN(base, PAGE_SIZE);
    end       = ALIGN_UP(end, PAGE_SIZE);

    pmm.reserved[pmm.reserved_count].start = start;
    pmm.reserved[pmm.reserved_count].end   = end;
    pmm.reserved_count++;

    u64 first = PMM_ADDR_TO_FRAME(start);
    u64 last  = PMM_ADDR_TO_FRAME(end);
    if (last > pmm.span_frames) {
        last = pmm.span_frames;
    }
    for (u64 f = first; f < last; f++) {
        frame_mark_used(f);
    }
    return true;
}

bool pmm_reserve_range(u64 base, u64 length) {
    if (!pmm.ready) {
        return false;
    }
    return reserve_range_internal(base, length);
}

static bool frame_is_reserved(u64 addr) {
    for (size_t i = 0; i < pmm.reserved_count; i++) {
        if (addr >= pmm.reserved[i].start && addr < pmm.reserved[i].end) {
            return true;
        }
    }
    return false;
}

/* True if the whole frame lies inside a USABLE region of the memory map. */
static bool frame_in_usable_region(u64 addr) {
    for (size_t i = 0; i < pmm.reclaimed_count; i++) {
        if (addr >= pmm.reclaimed[i].start && addr + PAGE_SIZE <= pmm.reclaimed[i].end) {
            return true;
        }
    }
    for (size_t i = 0; i < pmm.region_count; i++) {
        const mem_region_t *r = &pmm.regions[i];
        if (r->type != MEMT_USABLE) {
            continue;
        }
        if (addr >= r->base && addr + PAGE_SIZE <= r->base + r->length) {
            return true;
        }
    }
    return false;
}

/* =========================================================================
   INITIALISATION
   ========================================================================= */

bool pmm_init(const mem_info_t *mi, const pmm_plan_t *plan) {
    memset(&pmm, 0, sizeof pmm);

    if (mi == NULL || plan == NULL || !plan->found || mi->count == 0) {
        return false;
    }
    if ((plan->bitmap_phys & (PAGE_SIZE - 1)) != 0 ||
        plan->bitmap_bytes * 8 < plan->span_frames) {
        return false;
    }

    pmm.bitmap       = (u64 *)phys_to_virt(plan->bitmap_phys);
    pmm.bitmap_phys  = plan->bitmap_phys;
    pmm.bitmap_bytes = plan->bitmap_bytes;
    pmm.words        = plan->bitmap_bytes / sizeof(u64);
    pmm.span_frames  = plan->span_frames;
    pmm.regions      = mi->regions;
    pmm.region_count = mi->count;

    /* 1. Everything used (also covers the padding bits past span_frames). */
    memset(pmm.bitmap, 0xFF, plan->bitmap_bytes);

    /* 2. Free only whole frames inside USABLE regions. */
    for (size_t i = 0; i < mi->count; i++) {
        const mem_region_t *r = &mi->regions[i];
        if (r->type != MEMT_USABLE) {
            continue;
        }
        u64 first = PMM_ADDR_TO_FRAME(ALIGN_UP(r->base, PAGE_SIZE));
        u64 last  = PMM_ADDR_TO_FRAME(ALIGN_DOWN(r->base + r->length, PAGE_SIZE));
        for (u64 f = first; f < last && f < pmm.span_frames; f++) {
            frame_mark_free(f);
        }
    }
    pmm.total_frames = pmm.free_frames;

    /* 3. Carve out what we must never hand out. */
    if (!reserve_range_internal(0, PMM_LOW_RESERVE) ||
        !reserve_range_internal(plan->bitmap_phys, plan->bitmap_bytes)) {
        return false;
    }

    pmm.hint_word = 0;
    pmm.ready     = true;
    return true;
}

/* =========================================================================
   ALLOCATION
   ========================================================================= */

static paddr_t alloc_locked(void) {
    if (!pmm.ready) {
        return PMM_ALLOC_FAIL;
    }

    for (u64 w = pmm.hint_word; w < pmm.words; w++) {
        u64 v = pmm.bitmap[w];
        if (v == ~0ULL) {
            continue;                       /* 64 frames used: skip at once */
        }
        u64 frame = w * PMM_BITS_PER_WORD + (u64)__builtin_ctzll(~v);
        if (frame >= pmm.span_frames) {
            break;                          /* unreachable: padding bits are 1 */
        }
        frame_mark_used(frame);
        pmm.hint_word = w;
        return PMM_FRAME_TO_ADDR(frame);
    }

    pmm.hint_word = pmm.words;              /* nothing free; frees lower this */
    return PMM_ALLOC_FAIL;
}

paddr_t pmm_alloc_frame(void) {
    irq_flags_t fl = irq_save();
    paddr_t p = alloc_locked();
    irq_restore(fl);
    return p;
}

paddr_t pmm_alloc_frame_zeroed(void) {
    paddr_t p = pmm_alloc_frame();
    if (p != PMM_ALLOC_FAIL) {
        memset(phys_to_virt(p), 0, PAGE_SIZE);
    }
    return p;
}

paddr_t pmm_alloc_frame_or_panic(void) {
    paddr_t p = pmm_alloc_frame();
    if (p == PMM_ALLOC_FAIL) {
        kpanic("Out of physical memory (%lu frames total)", pmm.total_frames);
    }
    return p;
}

/* =========================================================================
   DEALLOCATION
   ========================================================================= */

const char *pmm_status_name(pmm_status_t st) {
    switch (st) {
    case PMM_OK:               return "ok";
    case PMM_ERR_NOT_INIT:     return "PMM not initialised";
    case PMM_ERR_UNALIGNED:    return "address not frame aligned";
    case PMM_ERR_OUT_OF_RANGE: return "address beyond managed memory";
    case PMM_ERR_RESERVED:     return "frame is reserved";
    case PMM_ERR_DOUBLE_FREE:  return "double free";
    default:                   return "unknown error";
    }
}

static pmm_status_t try_free_locked(paddr_t addr) {
    if (!pmm.ready) {
        return PMM_ERR_NOT_INIT;
    }
    if ((addr & (PAGE_SIZE - 1)) != 0) {
        return PMM_ERR_UNALIGNED;
    }
    u64 frame = PMM_ADDR_TO_FRAME(addr);
    if (frame >= pmm.span_frames) {
        return PMM_ERR_OUT_OF_RANGE;
    }
    if (frame_is_reserved(addr) || !frame_in_usable_region(addr)) {
        return PMM_ERR_RESERVED;
    }
    if (!frame_mark_free(frame)) {
        return PMM_ERR_DOUBLE_FREE;
    }
    return PMM_OK;
}

pmm_status_t pmm_try_free(paddr_t addr) {
    irq_flags_t fl = irq_save();
    pmm_status_t st = try_free_locked(addr);
    irq_restore(fl);
    return st;
}

bool pmm_free_frame(paddr_t addr) {
    pmm_status_t st = pmm_try_free(addr);

    if (st != PMM_OK) {
        kwarn("pmm_free_frame(0x%lx) refused: %s", addr, pmm_status_name(st));
        return false;
    }
    return true;
}

u64 pmm_reclaim_range(u64 base, u64 length) {
    if (!pmm.ready || length == 0) {
        return 0;
    }
    u64 start = ALIGN_UP(base, PAGE_SIZE);
    u64 end   = ALIGN_DOWN(base + length, PAGE_SIZE);
    if (end <= start || pmm.reclaimed_count >= PMM_MAX_RECLAIMED) {
        return 0;
    }
    irq_flags_t fl = irq_save();
    pmm.reclaimed[pmm.reclaimed_count].start = start;
    pmm.reclaimed[pmm.reclaimed_count].end   = end;
    pmm.reclaimed_count++;
    u64 n = 0;
    for (u64 a = start; a < end; a += PAGE_SIZE) {
        u64 f = PMM_ADDR_TO_FRAME(a);
        if (f >= pmm.span_frames) {
            break;
        }
        if (frame_is_reserved(a)) {          /* low 1 MiB and the bitmap stay reserved */
            continue;
        }
        if (frame_mark_free(f)) {
            n++;
        }
    }
    pmm.total_frames += n;                   /* reclaimed RAM is now managed RAM */
    irq_restore(fl);
    return n;
}

/* =========================================================================
   TRACKING
   ========================================================================= */

bool pmm_is_frame_used(paddr_t addr) {
    u64 frame = PMM_ADDR_TO_FRAME(addr);

    if (!pmm.ready || frame >= pmm.span_frames) {
        return true;    /* anything we do not manage is "not available" */
    }
    return frame_is_used(frame);
}

u64 pmm_get_total_frames(void) { return pmm.total_frames; }
u64 pmm_get_free_frames(void)  { return pmm.free_frames; }
u64 pmm_get_used_frames(void)  { return pmm.total_frames - pmm.free_frames; }

u64 pmm_get_total_memory_amount(void) { return PMM_FRAMES_TO_BYTES(pmm.total_frames); }
u64 pmm_get_free_memory_amount(void)  { return PMM_FRAMES_TO_BYTES(pmm.free_frames); }
u64 pmm_get_used_memory_amount(void)  { return PMM_FRAMES_TO_BYTES(pmm_get_used_frames()); }

void pmm_dump_stats(void) {
    char total[32], used[32], free_[32];

    fmt_size(total, sizeof total, pmm_get_total_memory_amount());
    fmt_size(used,  sizeof used,  pmm_get_used_memory_amount());
    fmt_size(free_, sizeof free_, pmm_get_free_memory_amount());

    kinfo("PMM: total %s (%lu frames) | used %s (%lu) | free %s (%lu)",
          total, pmm.total_frames,
          used,  pmm_get_used_frames(),
          free_, pmm.free_frames);
}

/* =========================================================================
   SELF-TEST
   ========================================================================= */

#define PMM_TEST_FRAMES 8

void pmm_test(void) {
    kinfo("PMM self-test: start");

    const u64 total0 = pmm_get_total_frames();
    const u64 free0  = pmm_get_free_frames();
    const u64 used0  = pmm_get_used_frames();
    KASSERT(free0 + used0 == total0);
    KASSERT(free0 >= PMM_TEST_FRAMES);

    /* ---- allocate ---- */
    paddr_t f[PMM_TEST_FRAMES];
    for (size_t i = 0; i < PMM_TEST_FRAMES; i++) {
        f[i] = pmm_alloc_frame();
        KASSERT(f[i] != PMM_ALLOC_FAIL);
        KASSERT((f[i] & (PAGE_SIZE - 1)) == 0);
        KASSERT(f[i] >= PMM_LOW_RESERVE);
        KASSERT(pmm_is_frame_used(f[i]));
        for (size_t j = 0; j < i; j++) {
            KASSERT(f[j] != f[i]);
        }
        if (i > 0) {
            KASSERT(f[i] > f[i - 1]);       /* first-fit with no frees between */
        }
    }
    KASSERT(pmm_get_free_frames() == free0 - PMM_TEST_FRAMES);
    KASSERT(pmm_get_used_frames() == used0 + PMM_TEST_FRAMES);

    /* ---- the memory is real and writable through the HHDM ---- */
    for (size_t i = 0; i < PMM_TEST_FRAMES; i++) {
        volatile u64 *p = phys_to_virt(f[i]);
        p[0]   = 0xDEADBEEF00000000ULL | i;
        p[511] = 0xCAFEBABE00000000ULL | i;
    }
    for (size_t i = 0; i < PMM_TEST_FRAMES; i++) {
        volatile u64 *p = phys_to_virt(f[i]);
        KASSERT(p[0]   == (0xDEADBEEF00000000ULL | i));
        KASSERT(p[511] == (0xCAFEBABE00000000ULL | i));
    }

    /* ---- free ---- */
    for (size_t i = 0; i < PMM_TEST_FRAMES; i++) {
        KASSERT(pmm_try_free(f[i]) == PMM_OK);
        KASSERT(!pmm_is_frame_used(f[i]));
    }
    KASSERT(pmm_get_free_frames() == free0);
    KASSERT(pmm_get_used_frames() == used0);

    /* ---- safety checks: all must be refused, counters untouched ---- */
    KASSERT(pmm_try_free(f[0])                 == PMM_ERR_DOUBLE_FREE);
    KASSERT(pmm_try_free(f[0] + 1)             == PMM_ERR_UNALIGNED);
    KASSERT(pmm_try_free(0)                    == PMM_ERR_RESERVED);   /* null page   */
    KASSERT(pmm_try_free(pmm.bitmap_phys)      == PMM_ERR_RESERVED);   /* the bitmap  */
    KASSERT(pmm_try_free(g_boot.kernel_phys_base) == PMM_ERR_RESERVED);/* the kernel  */
    KASSERT(pmm_try_free(PMM_FRAME_TO_ADDR(pmm.span_frames) + PAGE_SIZE)
                                               == PMM_ERR_OUT_OF_RANGE);
    KASSERT(pmm_get_free_frames() == free0);

    /* ---- lowest free frame is reused ---- */
    paddr_t again = pmm_alloc_frame();
    KASSERT(again == f[0]);
    KASSERT(pmm_try_free(again) == PMM_OK);

    /* ---- zeroed allocation really zeroes recycled memory ---- */
    paddr_t z = pmm_alloc_frame();
    KASSERT(z != PMM_ALLOC_FAIL);
    volatile u64 *zp = phys_to_virt(z);
    for (size_t i = 0; i < PAGE_SIZE / sizeof(u64); i++) {
        zp[i] = ~0ULL;
    }
    KASSERT(pmm_try_free(z) == PMM_OK);

    paddr_t z2 = pmm_alloc_frame_zeroed();
    KASSERT(z2 == z);
    for (size_t i = 0; i < PAGE_SIZE / sizeof(u64); i++) {
        KASSERT(zp[i] == 0);
    }
    KASSERT(pmm_try_free(z2) == PMM_OK);

    KASSERT(pmm_get_free_frames() == free0);
    KASSERT(pmm_get_used_frames() == used0);
    KASSERT(pmm_get_total_frames() == total0);

    kok("PMM self-test: passed (%u frames, counters consistent)",
        (unsigned)PMM_TEST_FRAMES);
}

/* =========================================================================
   CONTIGUOUS ALLOCATION (DMA buffers, page-table pools)
   Scans for a window of `count` free frames. When a used frame is found at
   the window's end, the next candidate starts right after it, so the scan
   never re-examines a frame more than `count` times.
   ========================================================================= */

paddr_t pmm_alloc_contiguous(u64 count, u64 max_end, u64 align) {
    if (!pmm.ready || count == 0 || count > pmm.span_frames) {
        return PMM_ALLOC_FAIL;
    }
    if (align < PAGE_SIZE) {
        align = PAGE_SIZE;
    }
    if ((align & (align - 1)) != 0) {
        return PMM_ALLOC_FAIL;
    }

    const u64 step  = align / PAGE_SIZE;
    u64       limit = pmm.span_frames;

    if (max_end != 0 && PMM_ADDR_TO_FRAME(max_end) < limit) {
        limit = PMM_ADDR_TO_FRAME(max_end);
    }
    if (count > limit) {
        return PMM_ALLOC_FAIL;
    }

    irq_flags_t fl = irq_save();
    paddr_t result = PMM_ALLOC_FAIL;
    u64 start = 0;

    while (start + count <= limit) {
        u64 i = count;
        while (i != 0 && !frame_is_used(start + i - 1)) {
            i--;
        }
        if (i == 0) {                               /* whole window is free */
            for (u64 f = start; f < start + count; f++) {
                frame_mark_used(f);
            }
            result = PMM_FRAME_TO_ADDR(start);
            break;
        }
        start = ALIGN_UP(start + i, step);          /* skip past the used frame */
    }
    irq_restore(fl);
    return result;
}

u64 pmm_free_contiguous(paddr_t addr, u64 count) {
    u64 freed = 0;

    for (u64 i = 0; i < count; i++) {
        if (pmm_try_free(addr + i * PAGE_SIZE) == PMM_OK) {
            freed++;
        }
    }
    return freed;
}