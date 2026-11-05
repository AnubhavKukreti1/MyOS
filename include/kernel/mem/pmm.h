#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PAGE_SIZE 4096

void pmm_init(void *mmap_request_response);
void *pmm_alloc_frame(void);
void pmm_free_frame(void *phys_addr);
void *pmm_alloc_frames(size_t count);
void pmm_free_frames(void *phys_addr, size_t count);

size_t pmm_get_total_memory(void);
size_t pmm_get_free_memory(void);
size_t pmm_get_used_memory(void);

bool pmm_try_free(uintptr_t phys_addr);

#endif
