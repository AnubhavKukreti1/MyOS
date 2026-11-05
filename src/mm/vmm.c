/*
 * MyOS - src/mm/vmm.c
 * x86_64 4-level paging. Physical table frames are reached through the HHDM.
 */

#include "kernel.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "kernel/spinlock.h"

extern char __kernel_start[], __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[], __data_start[], __kernel_end[];

/* PAT: 0 WB, 1 WT, 2 UC-, 3 UC, 4 WB, 5 WC, 6 UC-, 7 UC */
#define PAT_VALUE 0x0007010600070406ULL

static address_space_t g_kernel_as;
static bool            g_nx;

static inline pte_t *tbl(u64 phys) { return (pte_t *)phys_to_virt(phys); }

static bool canonical(u64 v) {
    u64 top = v >> 47;
    return top == 0 || top == 0x1FFFF;
}

static u64 leaf_flags(u64 flags) {
    flags &= ~(PTE_ADDR_MASK | VMM_NO_HUGE | PTE_PRESENT);
    if (!g_nx) {
        flags &= ~PTE_NX;
    }
    return flags | PTE_PRESENT;
}

static void tlb_flush(const address_space_t *as, u64 virt) {
    /* Kernel-half mappings are visible in every address space. */
    if (virt >= USER_SPACE_END || (read_cr3() & PTE_ADDR_MASK) == as->pml4_phys) {
        invlpg((const void *)(uintptr_t)virt);
    }
}

/* Intermediate entries are permissive; the leaf decides the final rights. */
static pte_t *next_table(pte_t *e, bool create, bool user) {
    if (*e & PTE_PRESENT) {
        if (*e & PTE_HUGE) {
            return NULL;                    /* a big page is in the way */
        }
        if (user && !(*e & PTE_USER)) {
            *e |= PTE_USER;
        }
        return tbl(*e & PTE_ADDR_MASK);
    }
    if (!create) {
        return NULL;
    }
    u64 p = pmm_alloc_frame_zeroed();
    if (p == PMM_ALLOC_FAIL) {
        return NULL;
    }
    *e = p | PTE_PRESENT | PTE_WRITE | (user ? PTE_USER : 0);
    return tbl(p);
}

/* level 0 -> the PTE (4 KiB), level 1 -> the PDE (2 MiB). */
static pte_t *walk(pte_t *pml4, u64 virt, int level, bool create, bool user) {
    const u64 idx[4] = { (virt >> 12) & 0x1FF, (virt >> 21) & 0x1FF,
                         (virt >> 30) & 0x1FF, (virt >> 39) & 0x1FF };
    pte_t *t = pml4;

    for (int l = 3; l > level; l--) {
        t = next_table(&t[idx[l]], create, user);
        if (t == NULL) {
            return NULL;
        }
    }
    return &t[idx[level]];
}

bool vmm_map_page_in(address_space_t *as, u64 virt, u64 phys, u64 flags) {
    if (!canonical(virt) || ((virt | phys) & (PAGE_SIZE - 1)) != 0) {
        return false;
    }
    bool user = (flags & PTE_USER) != 0;
    if (user && virt >= USER_SPACE_END) {
        return false;
    }
    pte_t *pte = walk(as->pml4, virt, 0, true, user);
    if (pte == NULL || (*pte & PTE_PRESENT)) {
        return false;                       /* never silently remap */
    }
    *pte = (phys & PTE_ADDR_MASK) | leaf_flags(flags);
    tlb_flush(as, virt);
    return true;
}

static bool map_2m(address_space_t *as, u64 virt, u64 phys, u64 flags) {
    pte_t *pde = walk(as->pml4, virt, 1, true, (flags & PTE_USER) != 0);
    if (pde == NULL || (*pde & PTE_PRESENT)) {
        return false;
    }
    *pde = (phys & PTE_ADDR_MASK) | leaf_flags(flags) | PTE_HUGE;
    return true;
}

bool vmm_map_range_in(address_space_t *as, u64 virt, u64 phys, u64 size, u64 flags) {
    const u64 huge = 2 * MiB;
    bool allow_huge = !(flags & VMM_NO_HUGE) && !(flags & PTE_PAT);

    if (((virt | phys | size) & (PAGE_SIZE - 1)) != 0) {
        return false;
    }
    while (size != 0) {
        if (allow_huge && size >= huge && ((virt | phys) & (huge - 1)) == 0) {
            if (!map_2m(as, virt, phys, flags)) {
                return false;
            }
            virt += huge; phys += huge; size -= huge;
        } else {
            if (!vmm_map_page_in(as, virt, phys, flags)) {
                return false;
            }
            virt += PAGE_SIZE; phys += PAGE_SIZE; size -= PAGE_SIZE;
        }
    }
    return true;
}

bool vmm_unmap_page_in(address_space_t *as, u64 virt, u64 *phys_out) {
    if (!canonical(virt) || (virt & (PAGE_SIZE - 1)) != 0) {
        return false;
    }
    pte_t *pte = walk(as->pml4, virt, 0, false, false);
    if (pte == NULL || !(*pte & PTE_PRESENT)) {
        return false;
    }
    if (phys_out != NULL) {
        *phys_out = *pte & PTE_ADDR_MASK;
    }
    *pte = 0;
    tlb_flush(as, virt);
    return true;                            /* empty tables are kept (simple, safe) */
}

/* flags: effective PTE_USER / PTE_WRITE (ANDed over all levels) plus leaf bits. */
bool vmm_translate_in(const address_space_t *as, u64 virt, u64 *phys, u64 *flags) {
    if (!canonical(virt)) {
        return false;
    }
    const u64 idx[4] = { (virt >> 12) & 0x1FF, (virt >> 21) & 0x1FF,
                         (virt >> 30) & 0x1FF, (virt >> 39) & 0x1FF };
    const pte_t *t = as->pml4;
    u64 eff = PTE_USER | PTE_WRITE;

    for (int l = 3; l >= 0; l--) {
        pte_t e = t[idx[l]];
        if (!(e & PTE_PRESENT)) {
            return false;
        }
        eff &= e;
        if (l == 0 || (l <= 2 && (e & PTE_HUGE))) {
            u64 span = 1ULL << (12 + 9 * l);
            if (phys != NULL) {
                *phys = ((e & PTE_ADDR_MASK) & ~(span - 1)) | (virt & (span - 1));
            }
            if (flags != NULL) {
                *flags = (e & ~(PTE_USER | PTE_WRITE | PTE_ADDR_MASK)) |
                         (eff & (PTE_USER | PTE_WRITE));
            }
            return true;
        }
        t = tbl(e & PTE_ADDR_MASK);
    }
    return false;
}

bool vmm_user_range_ok(const address_space_t *as, u64 virt, u64 len, bool write) {
    if (len == 0) {
        return true;
    }
    u64 end = virt + len;
    if (end < virt || end > USER_SPACE_END) {
        return false;
    }
    for (u64 p = ALIGN_DOWN(virt, PAGE_SIZE); p < end; p += PAGE_SIZE) {
        u64 fl;
        if (!vmm_translate_in(as, p, NULL, &fl) || !(fl & PTE_USER) ||
            (write && !(fl & PTE_WRITE))) {
            return false;
        }
    }
    return true;
}

/* ---- kernel-space wrappers ---------------------------------------------- */

address_space_t *vmm_kernel_space(void) { return &g_kernel_as; }
bool vmm_map_page(u64 virt, u64 phys, u64 flags) { return vmm_map_page_in(&g_kernel_as, virt, phys, flags); }
bool vmm_unmap_page(u64 virt) { return vmm_unmap_page_in(&g_kernel_as, virt, NULL); }
void vmm_switch(address_space_t *as) { write_cr3(as->pml4_phys); }

/* ---- per-process address spaces ----------------------------------------- */

address_space_t *vmm_create_space(void) {
    address_space_t *as = kmalloc(sizeof *as);
    if (as == NULL) {
        return NULL;
    }
    u64 p = pmm_alloc_frame_zeroed();
    if (p == PMM_ALLOC_FAIL) {
        kfree(as);
        return NULL;
    }
    as->pml4_phys = p;
    as->pml4      = tbl(p);
    for (int i = 256; i < 512; i++) {       /* share the kernel half */
        as->pml4[i] = g_kernel_as.pml4[i];
    }
    return as;
}

/* l = level of the table at table_phys (0 = PT). Leaf frames are owned. */
static void free_table_tree(u64 table_phys, int l) {
    pte_t *t = tbl(table_phys);

    for (int i = 0; i < 512; i++) {
        pte_t e = t[i];
        if (!(e & PTE_PRESENT)) {
            continue;
        }
        if (l == 0) {
            pmm_free_frame(e & PTE_ADDR_MASK);
        } else if (!(e & PTE_HUGE)) {
            free_table_tree(e & PTE_ADDR_MASK, l - 1);
        }
    }
    pmm_free_frame(table_phys);
}

void vmm_destroy_space(address_space_t *as) {
    KASSERT(as != &g_kernel_as);
    KASSERT((read_cr3() & PTE_ADDR_MASK) != as->pml4_phys);

    for (int i = 0; i < 256; i++) {
        if (as->pml4[i] & PTE_PRESENT) {
            free_table_tree(as->pml4[i] & PTE_ADDR_MASK, 2);
        }
    }
    pmm_free_frame(as->pml4_phys);
    kfree(as);
}

/* ---- building the kernel's own tables ----------------------------------- */

static void map_section(const boot_info_t *bi, void *start, void *end, u64 flags) {
    u64 v0 = ALIGN_DOWN((u64)(uintptr_t)start, PAGE_SIZE);
    u64 v1 = ALIGN_UP((u64)(uintptr_t)end, PAGE_SIZE);
    u64 p0 = v0 - bi->kernel_virt_base + bi->kernel_phys_base;

    if (v1 > v0 && !vmm_map_range_in(&g_kernel_as, v0, p0, v1 - v0, flags | VMM_NO_HUGE)) {
        kpanic("VMM: cannot map kernel section %p..%p", start, end);
    }
}

void vmm_init(const boot_info_t *bi) {
    u32 a, b, c, d;

    cpuid(0x80000000u, 0, &a, &b, &c, &d);
    if (a >= 0x80000001u) {
        cpuid(0x80000001u, 0, &a, &b, &c, &d);
        g_nx = ((d >> 20) & 1) != 0;
    }
    if (g_nx) {
        wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);   /* before any NX bit is used */
    }
    write_cr0(read_cr0() | CR0_WP);                     /* ring 0 honours read-only pages */

    g_kernel_as.pml4_phys = pmm_alloc_frame_zeroed();
    KASSERT(g_kernel_as.pml4_phys != PMM_ALLOC_FAIL);
    g_kernel_as.pml4 = tbl(g_kernel_as.pml4_phys);

    /* Fix the whole kernel half now so every future address space shares it. */
    for (int i = 256; i < 512; i++) {
        u64 p = pmm_alloc_frame_zeroed();
        KASSERT(p != PMM_ALLOC_FAIL);
        g_kernel_as.pml4[i] = p | PTE_PRESENT | PTE_WRITE;
    }

    /* Higher-half direct map: only memory that really exists. */
    for (size_t i = 0; i < bi->region_count; i++) {
        const mem_region_t *r = &bi->regions[i];
        u64 flags = VMM_KERNEL_RW;

        switch (r->type) {
        case MEMT_USABLE:
        case MEMT_BOOTLOADER_RECLAIMABLE:
        case MEMT_KERNEL_AND_MODULES:
        case MEMT_ACPI_RECLAIMABLE:
        case MEMT_ACPI_NVS:
            break;
        case MEMT_FRAMEBUFFER:
            flags |= PTE_WC4K | VMM_NO_HUGE;
            break;
        default:
            continue;                       /* RESERVED / BAD: never mapped */
        }
        u64 base = ALIGN_DOWN(r->base, PAGE_SIZE);
        u64 end  = ALIGN_UP(r->base + r->length, PAGE_SIZE);
        if (!vmm_map_range_in(&g_kernel_as, bi->hhdm_offset + base, base, end - base, flags)) {
            kpanic("VMM: cannot map HHDM region #%zu", i);
        }
    }

    /* Kernel image with W^X. The first section holds Limine's request data. */
    map_section(bi, __kernel_start, __text_start,   VMM_KERNEL_RW);
    map_section(bi, __text_start,   __text_end,     VMM_KERNEL_RX);
    map_section(bi, __rodata_start, __rodata_end,   VMM_KERNEL_RO);
    map_section(bi, __data_start,   __kernel_end,   VMM_KERNEL_RW);

    __asm__ volatile ("wbinvd" : : : "memory");     /* memory types are about to change */
    wrmsr(MSR_PAT, PAT_VALUE);
    write_cr3(g_kernel_as.pml4_phys);

    kok("VMM: own page tables active (CR3=0x%lx, NX=%s)",
        g_kernel_as.pml4_phys, g_nx ? "yes" : "no");
}

/* ---- MMIO window --------------------------------------------------------- */
static spinlock_t g_mmio_lock = SPINLOCK_INIT;
static u64        g_mmio_cursor = VMM_MMIO_BASE;

static bool phys_in_hhdm(u64 phys, u64 len) {
    for (size_t i = 0; i < g_boot.region_count; i++) {
        const mem_region_t *r = &g_boot.regions[i];
        switch (r->type) {
        case MEMT_USABLE:
        case MEMT_BOOTLOADER_RECLAIMABLE:
        case MEMT_KERNEL_AND_MODULES:
        case MEMT_ACPI_RECLAIMABLE:
        case MEMT_ACPI_NVS:
            break;
        default:
            continue;
        }
        if (phys >= r->base && phys + len <= r->base + r->length) {
            return true;
        }
    }
    return false;
}

void *vmm_map_mmio(u64 phys, size_t size) {
    if (size == 0 || phys + size < phys) {
        return NULL;
    }
    if (phys_in_hhdm(phys, size)) {
        return phys_to_virt(phys);
    }
    
    const u64 base  = ALIGN_DOWN(phys, PAGE_SIZE);
    const u64 off   = phys - base;
    const u64 len   = ALIGN_UP(off + size, PAGE_SIZE);
    const u64 align = (len >= (2 * MiB)) ? (2 * MiB) : PAGE_SIZE;

    irq_flags_t fl = spin_lock_irqsave(&g_mmio_lock);
    
    /* Same low bits for VA and PA so 2 MiB pages stay usable. */
    u64 va = ALIGN_UP(g_mmio_cursor, align) + (base & (align - 1));
    if (va + len > VMM_MMIO_BASE + VMM_MMIO_SIZE) {
        spin_unlock_irqrestore(&g_mmio_lock, fl);
        return NULL;
    }
    
    g_mmio_cursor = va + len;
    spin_unlock_irqrestore(&g_mmio_lock, fl);

    /* PWT|PCD with PAT bit clear = PAT index 3 = UC in our PAT layout. */
    if (!vmm_map_range_in(&g_kernel_as, va, base, len,
                          VMM_KERNEL_RW | PTE_PCD | PTE_PWT)) {
        return NULL;
    }
    
    return (void *)(uintptr_t)(va + off);
}