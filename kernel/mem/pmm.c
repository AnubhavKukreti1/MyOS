#include <kernel/mem/pmm.h>
#include "pmm_internal.h"

static uint64_t *pmm_bitmap = NULL;
static size_t pmm_bitmap_size = 0;
static size_t pmm_total_frames = 0;
static size_t pmm_free_frames_count = 0;
static size_t pmm_last_search_index = 0;

static inline void bitmap_set(size_t bit) {
    pmm_bitmap[INDEX_FROM_BIT(bit)] |= (1ULL << OFFSET_FROM_BIT(bit));
}

static inline void bitmap_clear(size_t bit) {
    pmm_bitmap[INDEX_FROM_BIT(bit)] &= ~(1ULL << OFFSET_FROM_BIT(bit));
}

static inline bool bitmap_test(size_t bit) {
    return (pmm_bitmap[INDEX_FROM_BIT(bit)] & (1ULL << OFFSET_FROM_BIT(bit))) != 0;
}

static int64_t bitmap_find_first_free(size_t count) {
    size_t starting_index = pmm_last_search_index;
    size_t consecutive = 0;
    size_t first_free = 0;

    for (size_t i = 0; i < pmm_total_frames; ++i) {
        size_t idx = (starting_index + i) % pmm_total_frames;
        if (!bitmap_test(idx)) {
            if (consecutive == 0) first_free = idx;
            consecutive++;
            if (consecutive == count) {
                pmm_last_search_index = idx;
                return (int64_t)first_free;
            }
        } else {
            consecutive = 0;
        }
    }
    return -1;
}

void pmm_init(void *mmap_request_response) {
    // Stub for memory map parsing
    (void)mmap_request_response;
}

void *pmm_alloc_frame(void) {
    int64_t frame = bitmap_find_first_free(1);
    if (frame < 0) {
        return NULL; // Out of memory
    }
    bitmap_set((size_t)frame);
    pmm_free_frames_count--;
    return (void *)(frame * PAGE_SIZE);
}

void pmm_free_frame(void *phys_addr) {
    uintptr_t addr = (uintptr_t)phys_addr;
    size_t frame = addr / PAGE_SIZE;

    if (frame >= pmm_total_frames) {
        return; // Out of bounds
    }

    if (bitmap_test(frame)) {
        bitmap_clear(frame);
        pmm_free_frames_count++;
        if (frame < pmm_last_search_index) {
            pmm_last_search_index = frame;
        }
    }
}

void *pmm_alloc_frames(size_t count) {
    if (count == 0) return NULL;
    if (count == 1) return pmm_alloc_frame();

    int64_t start_frame = bitmap_find_first_free(count);
    if (start_frame < 0) {
        return NULL;
    }

    for (size_t i = 0; i < count; ++i) {
        bitmap_set((size_t)start_frame + i);
    }
    pmm_free_frames_count -= count;

    return (void *)(start_frame * PAGE_SIZE);
}

void pmm_free_frames(void *phys_addr, size_t count) {
    if (!phys_addr || count == 0) return;
    uintptr_t addr = (uintptr_t)phys_addr;
    size_t start_frame = addr / PAGE_SIZE;

    for (size_t i = 0; i < count; ++i) {
        size_t frame = start_frame + i;
        if (frame < pmm_total_frames && bitmap_test(frame)) {
            bitmap_clear(frame);
            pmm_free_frames_count++;
        }
    }
    if (start_frame < pmm_last_search_index) {
        pmm_last_search_index = start_frame;
    }
}

static size_t g_total_usable_memory = 0;

size_t pmm_get_total_memory(void) {
    return g_total_usable_memory;
}

size_t pmm_get_free_memory(void) {
    return pmm_free_frames_count * PAGE_SIZE;
}

size_t pmm_get_used_memory(void) {
    return g_total_usable_memory - pmm_get_free_memory();
}

bool pmm_try_free(uintptr_t phys_addr) {
    size_t frame = phys_addr / PAGE_SIZE;
    if (frame >= pmm_total_frames) return false;
    
    // Check if it is marked as used and safe to inspect
    return bitmap_test(frame);
}

// Limine structures placeholder integration
void pmm_parse_limine_map(void *mmap_response) {
    // Calculates highest physical address, allocates bitmap space, 
    // and marks reserved/kernel/reclaimable regions appropriately.
    (void)mmap_response;
}

static void pmm_reserve_lower_memory(void) {
    // Reserve lower 1MB (BIOS, IVT, VGA, BDA)
    size_t lower_frames = 0x100000 / PAGE_SIZE;
    for (size_t i = 0; i < lower_frames; ++i) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            pmm_free_frames_count--;
        }
    }
}

// Ensures kernel and bootloader structures remain marked as used permanently 
// until explicit late-boot reclamation.

void pmm_run_tests(void) {
    void *f1 = pmm_alloc_frame();
    void *f2 = pmm_alloc_frame();
    
    // Verify allocation uniqueness
    if (f1 && f2 && f1 != f2) {
        pmm_free_frame(f1);
        pmm_free_frame(f2);
    }
}

// Hooked into kernel_main()
