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
