#ifndef PMM_INTERNAL_H
#define PMM_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

#define INDEX_FROM_BIT(a) (a / (8 * sizeof(uint64_t)))
#define OFFSET_FROM_BIT(a) (a % (8 * sizeof(uint64_t)))

#endif
