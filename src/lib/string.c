/*
 * MyOS - src/lib/string.c
 * Part of the MyOS 0.2.0 kernel (split out of the monolithic main.c).
 */

/* #########################################################################
   FREESTANDING LIBC SUBSET
   -------------------------------------------------------------------------
   Implemented with `rep` string instructions rather than C loops. A plain C
   loop can be "recognised" by GCC and turned back into a call to memcpy,
   which would recurse infinitely inside memcpy itself. Inline asm cannot be
   pattern-matched, so this is safe at any -O level.
   ######################################################################### */

#include "kernel.h"

void *memcpy(void *restrict dest, const void *restrict src, size_t n) {
    void *d = dest;
    __asm__ volatile ("rep movsb"
                      : "+D"(d), "+S"(src), "+c"(n)
                      :
                      : "memory");
    return dest;
}

void *memset(void *dest, int value, size_t n) {
    void *d = dest;
    __asm__ volatile ("rep stosb"
                      : "+D"(d), "+c"(n)
                      : "a"(value)
                      : "memory");
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    if ((uintptr_t)dest == (uintptr_t)src || n == 0) {
        return dest;
    }

    if ((uintptr_t)dest < (uintptr_t)src ||
        (uintptr_t)dest >= (uintptr_t)src + n) {
        /* No harmful overlap: forward copy is correct. */
        void *d = dest;
        const void *s = src;
        __asm__ volatile ("rep movsb"
                          : "+D"(d), "+S"(s), "+c"(n)
                          :
                          : "memory");
    } else {
        /* dest overlaps the tail of src: copy backwards (DF=1). */
        void *d = (u8 *)dest + n - 1;
        const void *s = (const u8 *)src + n - 1;
        __asm__ volatile ("std; rep movsb; cld"
                          : "+D"(d), "+S"(s), "+c"(n)
                          :
                          : "memory", "cc");
    }
    return dest;
}

int memcmp(const void *a, const void *b, size_t n) {
    const u8 *pa = a;
    const u8 *pb = b;

    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

size_t kstrlen(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}