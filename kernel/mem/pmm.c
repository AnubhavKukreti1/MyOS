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
