#ifndef MYOS_MM_HEAP_H
#define MYOS_MM_HEAP_H

#include "kernel.h"

void  heap_init(void);                  /* needs PMM + VMM */
void *kmalloc(size_t size);             /* 16-byte aligned, NULL on failure */
void *kzalloc(size_t size);
void  kfree(void *ptr);                 /* kfree(NULL) is a no-op */
u64   heap_used_bytes(void);
u64   heap_total_bytes(void);
void  heap_dump_stats(void);
void  heap_test(void);

#endif