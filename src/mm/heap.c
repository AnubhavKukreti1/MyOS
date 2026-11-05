/*
 * MyOS - src/mm/heap.c
 * Invariants: blocks tile [base, end) with no gaps; no two free blocks are
 * adjacent (always coalesced); header size keeps payloads 16-byte aligned.
 */

#include "kernel.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"

#define HEAP_BASE       0xFFFFC00000000000ULL
#define HEAP_MAX_SIZE   (1ULL * GiB)
#define HEAP_GROW_MIN   (64ULL * KiB)
#define HEAP_ALIGN      16u
#define MAGIC_USED      0xA110C8EDA110C8EDULL
#define MAGIC_FREE      0xF4EEF4EEF4EEF4EEULL

typedef struct block {
    u64           magic;
    u64           size;          /* payload bytes */
    struct block *prev;          /* physically preceding block, NULL for the first */
    u64           pad;
    struct block *next_free;     /* payload starts here; links valid only when free */
    struct block *prev_free;
} block_t;

#define HDR_SIZE     offsetof(block_t, next_free)    /* 32 */
#define MIN_PAYLOAD  (2 * sizeof(void *))

static struct {
    bool     ready;
    u8      *base, *end;
    block_t *head;               /* free list */
    block_t *last;               /* physically last block */
    u64      used;
} heap;

static void free_insert(block_t *b) {
    b->prev_free = NULL;
    b->next_free = heap.head;
    if (heap.head != NULL) {
        heap.head->prev_free = b;
    }
    heap.head = b;
}

static void free_remove(block_t *b) {
    if (b->prev_free != NULL) {
        b->prev_free->next_free = b->next_free;
    } else {
        heap.head = b->next_free;
    }
    if (b->next_free != NULL) {
        b->next_free->prev_free = b->prev_free;
    }
}

static block_t *next_phys(const block_t *b) {
    u8 *n = (u8 *)b + HDR_SIZE + b->size;
    return (n < heap.end) ? (block_t *)n : NULL;
}

/* b is NOT on the free list. Merges with free neighbours, then lists it. */
static void coalesce_and_insert(block_t *b) {
    block_t *n = next_phys(b);
    if (n != NULL && n->magic == MAGIC_FREE) {
        free_remove(n);
        b->size += HDR_SIZE + n->size;
        if (heap.last == n) {
            heap.last = b;
        }
        block_t *nn = next_phys(b);
        if (nn != NULL) {
            nn->prev = b;
        }
    }
    block_t *p = b->prev;
    if (p != NULL && p->magic == MAGIC_FREE) {
        free_remove(p);
        p->size += HDR_SIZE + b->size;
        if (heap.last == b) {
            heap.last = p;
        }
        block_t *nn = next_phys(p);
        if (nn != NULL) {
            nn->prev = p;
        }
        b->magic = 0;
        b = p;
    }
    b->magic = MAGIC_FREE;
    free_insert(b);
}

static bool heap_grow(u64 need_payload) {
    u64 bytes = ALIGN_UP(need_payload + HDR_SIZE, PAGE_SIZE);
    if (bytes < HEAP_GROW_MIN) {
        bytes = HEAP_GROW_MIN;
    }
    if ((u64)(heap.end - heap.base) + bytes > HEAP_MAX_SIZE) {
        return false;
    }

    u8 *start = heap.end;
    for (u64 done = 0; done < bytes; done += PAGE_SIZE) {
        paddr_t f = pmm_alloc_frame();
        if (f == PMM_ALLOC_FAIL ||
            !vmm_map_page((u64)(uintptr_t)(start + done), f, VMM_KERNEL_RW)) {
            if (f != PMM_ALLOC_FAIL) {
                pmm_free_frame(f);
            }
            while (done != 0) {             /* roll back what we mapped */
                u64 p;
                done -= PAGE_SIZE;
                if (vmm_unmap_page_in(vmm_kernel_space(), (u64)(uintptr_t)(start + done), &p)) {
                    pmm_free_frame(p);
                }
            }
            return false;
        }
    }

    block_t *b = (block_t *)start;
    b->size = bytes - HDR_SIZE;
    b->prev = heap.last;
    heap.end += bytes;
    heap.last = b;
    coalesce_and_insert(b);
    return true;
}

void heap_init(void) {
    heap.base = heap.end = (u8 *)(uintptr_t)HEAP_BASE;
    if (!heap_grow(HEAP_GROW_MIN - HDR_SIZE)) {
        kpanic("heap_init: cannot map the initial heap");
    }
    heap.ready = true;
    kok("Heap online at %p (%lu KiB)", (void *)heap.base, heap_total_bytes() / KiB);
}

static block_t *find_fit(u64 want) {
    for (block_t *b = heap.head; b != NULL; b = b->next_free) {
        if (b->size >= want) {
            return b;
        }
    }
    return NULL;
}

static void split(block_t *b, u64 want) {
    if (b->size < want + HDR_SIZE + MIN_PAYLOAD) {
        return;
    }
    block_t *r = (block_t *)((u8 *)b + HDR_SIZE + want);
    r->size = b->size - want - HDR_SIZE;
    r->prev = b;
    b->size = want;
    block_t *n = next_phys(r);
    if (n != NULL) {
        n->prev = r;
    }
    if (heap.last == b) {
        heap.last = r;
    }
    r->magic = MAGIC_FREE;
    free_insert(r);
}

void *kmalloc(size_t size) {
    if (!heap.ready || size == 0 || size > HEAP_MAX_SIZE / 2) {
        return NULL;
    }
    u64 want = ALIGN_UP(size, HEAP_ALIGN);
    if (want < MIN_PAYLOAD) {
        want = MIN_PAYLOAD;
    }

    irq_flags_t fl = irq_save();
    block_t *b = find_fit(want);
    if (b == NULL && heap_grow(want)) {
        b = find_fit(want);
    }
    if (b == NULL) {
        irq_restore(fl);
        return NULL;
    }
    free_remove(b);
    split(b, want);
    b->magic = MAGIC_USED;
    heap.used += b->size;
    irq_restore(fl);
    return (u8 *)b + HDR_SIZE;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p != NULL) {
        memset(p, 0, size);
    }
    return p;
}

void kfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    u8 *u = ptr;
    if (u < heap.base + HDR_SIZE || u >= heap.end || ((uintptr_t)u & (HEAP_ALIGN - 1)) != 0) {
        kpanic("kfree: %p is not a heap pointer", ptr);
    }
    block_t *b = (block_t *)(u - HDR_SIZE);
    if (b->magic == MAGIC_FREE) {
        kpanic("kfree: double free of %p", ptr);
    }
    if (b->magic != MAGIC_USED) {
        kpanic("kfree: bad header for %p (corruption or foreign pointer)", ptr);
    }

    irq_flags_t fl = irq_save();
    heap.used -= b->size;
    coalesce_and_insert(b);
    irq_restore(fl);
}

u64 heap_used_bytes(void)  { return heap.used; }
u64 heap_total_bytes(void) { return (u64)(heap.end - heap.base); }

void heap_dump_stats(void) {
    kinfo("Heap: %lu bytes used of %lu mapped", heap.used, heap_total_bytes());
}

void heap_test(void) {
    const u64 used0 = heap.used;

    void *a = kmalloc(1), *b = kmalloc(100), *c = kmalloc(5000);
    KASSERT(a != NULL && b != NULL && c != NULL);
    KASSERT((((uintptr_t)a | (uintptr_t)b | (uintptr_t)c) & 15) == 0);
    KASSERT(a != b && b != c && a != c);
    memset(a, 0xAA, 1); memset(b, 0xBB, 100); memset(c, 0xCC, 5000);

    kfree(b);
    void *d = kmalloc(64);                  /* must reuse b's hole */
    KASSERT(d == b);
    kfree(a); kfree(c); kfree(d);
    KASSERT(heap.used == used0);

    u64 total0 = heap_total_bytes();
    u8 *big = kmalloc(300 * KiB);           /* forces the heap to grow */
    KASSERT(big != NULL && heap_total_bytes() > total0);
    memset(big, 0x5A, 300 * KiB);
    kfree(big);
    KASSERT(heap.used == used0);

    void *p[64];
    for (int i = 0; i < 64; i++) {
        p[i] = kmalloc(8 + (size_t)i * 8);
        KASSERT(p[i] != NULL);
    }
    for (int i = 0; i < 64; i += 2) { kfree(p[i]); }
    for (int i = 1; i < 64; i += 2) { kfree(p[i]); }
    KASSERT(heap.used == used0);

    kok("heap self-test: passed");
}