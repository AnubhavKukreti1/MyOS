/* MyOS - src/kernel/process.c */

#include "kernel.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "kernel/process.h"

static u32 g_next_pid = 1;

void process_destroy(process_t *p) {
    if (p->as != NULL) {
        vmm_destroy_space(p->as);         /* frees every user frame and table */
    }
    kfree(p);
}

static bool map_user_page(address_space_t *as, u64 va, u64 flags, const void *src, size_t n) {
    paddr_t f = pmm_alloc_frame_zeroed();
    if (f == PMM_ALLOC_FAIL) {
        return false;
    }
    if (src != NULL && n != 0) {
        memcpy(phys_to_virt(f), src, n);
    }
    if (!vmm_map_page_in(as, va, f, flags)) {
        pmm_free_frame(f);
        return false;
    }
    return true;
}

process_t *process_create_user(const char *name, const void *image, size_t size, u64 entry_offset) {
    process_t *p = kzalloc(sizeof *p);
    if (p == NULL) {
        return NULL;
    }
    name_copy(p->name, name, sizeof p->name);

    irq_flags_t fl = irq_save();
    p->pid = g_next_pid++;
    irq_restore(fl);

    p->as = vmm_create_space();
    if (p->as == NULL) {
        kfree(p);
        return NULL;
    }

    const u8 *src = image;
    for (u64 off = 0; off < size; off += PAGE_SIZE) {
        size_t n = (size - off < PAGE_SIZE) ? (size_t)(size - off) : PAGE_SIZE;
        if (!map_user_page(p->as, USER_CODE_BASE + off, VMM_USER_RX, src + off, n)) {
            goto fail;
        }
    }
    for (u64 i = 1; i <= USER_STACK_PAGES; i++) {
        if (!map_user_page(p->as, USER_STACK_TOP - i * PAGE_SIZE, VMM_USER_RW, NULL, 0)) {
            goto fail;
        }
    }
    if (sched_spawn_user(name, p, USER_CODE_BASE + entry_offset, USER_STACK_TOP) == NULL) {
        goto fail;
    }
    kok("Process '%s' (pid %u) created: %lu bytes at 0x%lx",
        p->name, (unsigned)p->pid, (unsigned long)size, (unsigned long)USER_CODE_BASE);
    return p;

fail:
    process_destroy(p);
    return NULL;
}