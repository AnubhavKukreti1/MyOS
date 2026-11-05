#ifndef MYOS_MM_VMM_H
#define MYOS_MM_VMM_H

#include "kernel.h"
#include "kernel/spinlock.h"

typedef u64 pte_t;

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITE     (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PWT       (1ULL << 3)
#define PTE_PCD       (1ULL << 4)
#define PTE_ACCESSED  (1ULL << 5)
#define PTE_DIRTY     (1ULL << 6)
#define PTE_HUGE      (1ULL << 7)     /* PS bit in PDE/PDPTE (2 MiB / 1 GiB)   */
#define PTE_PAT       (1ULL << 7)     /* PAT bit in a 4 KiB PTE (same bit)     */
#define PTE_GLOBAL    (1ULL << 8)
#define PTE_NX        (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define VMM_NO_HUGE   (1ULL << 9)     /* software hint, never stored in a PTE  */
#define PTE_WC4K      (PTE_PWT | PTE_PAT)   /* write-combining (PAT index 5), 4 KiB only */

#define VMM_KERNEL_RX 0ULL
#define VMM_KERNEL_RO (PTE_NX)
#define VMM_KERNEL_RW (PTE_WRITE | PTE_NX)
#define VMM_USER_RX   (PTE_USER)
#define VMM_USER_RO   (PTE_USER | PTE_NX)
#define VMM_USER_RW   (PTE_USER | PTE_WRITE | PTE_NX)

#define USER_SPACE_END 0x0000800000000000ULL

typedef struct {
    pte_t *pml4;        /* HHDM virtual address of the top-level table */
    u64    pml4_phys;
} address_space_t;

/* Build the kernel's own tables and switch CR3 to them. PMM must be ready. */
void vmm_init(const boot_info_t *bi);
address_space_t *vmm_kernel_space(void);

bool vmm_map_page_in(address_space_t *as, u64 virt, u64 phys, u64 flags);
bool vmm_unmap_page_in(address_space_t *as, u64 virt, u64 *phys_out);
bool vmm_map_range_in(address_space_t *as, u64 virt, u64 phys, u64 size, u64 flags);
bool vmm_translate_in(const address_space_t *as, u64 virt, u64 *phys, u64 *flags);
bool vmm_user_range_ok(const address_space_t *as, u64 virt, u64 len, bool write);

/* Convenience wrappers for the kernel address space. */
bool vmm_map_page(u64 virt, u64 phys, u64 flags);
bool vmm_unmap_page(u64 virt);

/* Process address spaces share the kernel half. They OWN every user frame
   mapped into them: vmm_destroy_space() returns those frames to the PMM. */
address_space_t *vmm_create_space(void);
void vmm_destroy_space(address_space_t *as);
void* vmm_map_mmio(uint64_t phys_addr, size_t size);
void vmm_switch(address_space_t *as);

#define VMM_MMIO_BASE 0xFFFFD00000000000ULL
#define VMM_MMIO_SIZE (1ULL << 40)

/**
 * @brief Map a physical range (device BAR, ECAM, firmware ROM) uncached.
 *
 * Ranges already covered by the HHDM (e.g. ACPI tables in reclaimable memory)
 * are returned through it instead. Mappings are permanent. 2 MiB pages are
 * used when alignment allows, so mapping a 256 MiB ECAM window is cheap.
 *
 * @return virtual address of `phys` (page offset preserved) or NULL.
 */
void *vmm_map_mmio(u64 phys, size_t size);

#endif /* MYOS_MM_VMM_H */