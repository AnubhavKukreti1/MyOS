/*
 * MyOS - src/boot/boot.c
 * Part of the MyOS 0.2.0 kernel (split out of the monolithic main.c).
 */

/* #########################################################################
   BOOT PROTOCOL VALIDATION (Limine)
   -------------------------------------------------------------------------
   boot_collect() is table-driven: every request gets a named check with a
   `fatal` flag and a human-readable detail string. Responses are validated
   for CONTENT (not just non-NULL) and copied out, so later code never has to
   dereference bootloader-owned memory - which matters because that memory
   (BOOTLOADER_RECLAIMABLE) is eventually handed to the PMM.
   ######################################################################### */

/*
 * Limine requests. Requests are static volatile objects placed in a
 * dedicated section. The bootloader scans the kernel image for them and
 * fills in .response before jumping to us. `volatile` stops the compiler
 * from constant-folding the NULL initialiser. They are private to this file
 * on purpose: the rest of the kernel consumes boot_info_t, never raw Limine
 * structures.
 */

#define LIMINE_API_REVISION 3

#include "mm/pmm.h"    /* Added for pmm_reclaim_range() and PAGE_SIZE */
#include <limine.h>

#include "kernel.h"

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3)

__attribute__((used, section(".limine_requests_start"), aligned(8)))
static volatile LIMINE_REQUESTS_START_MARKER

__attribute__((used, section(".limine_requests_end"), aligned(8)))
static volatile LIMINE_REQUESTS_END_MARKER

__attribute__((used, section(".limine_requests"), aligned(8)))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests"), aligned(8)))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests"), aligned(8)))
static volatile struct limine_executable_address_request kernel_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST,
    .revision = 0,
    .response = NULL
};

__attribute__((used, section(".limine_requests"), aligned(8)))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
    .response = NULL
};

/* Kernel-owned copy of everything we learned from the bootloader. */
boot_info_t g_boot;

static void boot_set(boot_info_t *bi, boot_check_id_t id, const char *name,
                     bool fatal, const char *detail) {
    bi->checks[id].name   = name;
    bi->checks[id].fatal  = fatal;
    bi->checks[id].ok     = false;
    bi->checks[id].detail = detail;
}

static void boot_pass(boot_info_t *bi, boot_check_id_t id, const char *detail) {
    bi->checks[id].ok     = true;
    bi->checks[id].detail = detail;
}

static void boot_fail(boot_info_t *bi, boot_check_id_t id, const char *detail) {
    bi->checks[id].ok     = false;
    bi->checks[id].detail = detail;
}

static mem_type_t translate_memmap_type(u64 type) {
    switch (type) {
    case LIMINE_MEMMAP_USABLE:                  return MEMT_USABLE;
    case LIMINE_MEMMAP_RESERVED:                return MEMT_RESERVED;
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:        return MEMT_ACPI_RECLAIMABLE;
    case LIMINE_MEMMAP_ACPI_NVS:                return MEMT_ACPI_NVS;
    case LIMINE_MEMMAP_BAD_MEMORY:              return MEMT_BAD;
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:  return MEMT_BOOTLOADER_RECLAIMABLE;
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:  return MEMT_KERNEL_AND_MODULES;
    case LIMINE_MEMMAP_FRAMEBUFFER:             return MEMT_FRAMEBUFFER;
    default:                                    return MEMT_UNKNOWN;
    }
}

void boot_collect(boot_info_t *bi) {
    memset(bi, 0, sizeof *bi);

    boot_set(bi, BOOT_CHK_BASE_REV,    "Limine base revision", true,  "not checked");
    boot_set(bi, BOOT_CHK_HHDM,        "HHDM",                 true,  "no response");
    boot_set(bi, BOOT_CHK_MEMMAP,      "Memory map",           true,  "no response");
    boot_set(bi, BOOT_CHK_KADDR,       "Kernel address",       true,  "no response");
    boot_set(bi, BOOT_CHK_FRAMEBUFFER, "Framebuffer",          false, "no response");

    /* If the bootloader does not speak our base revision, none of the
     * responses can be trusted - bail out and let the report explain. */
    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        boot_fail(bi, BOOT_CHK_BASE_REV,
                  "bootloader does not support base revision 3 (update Limine)");
        return;
    }
    boot_pass(bi, BOOT_CHK_BASE_REV, "revision 3 accepted");

    /* ---- HHDM ---- */
    const struct limine_hhdm_response *hhdm = hhdm_request.response;
    if (hhdm != NULL) {
        if (hhdm->offset < HIGHER_HALF_START) {
            boot_fail(bi, BOOT_CHK_HHDM, "offset is not in the higher half");
        } else {
            bi->hhdm_offset = hhdm->offset;
            boot_pass(bi, BOOT_CHK_HHDM, "direct map present");
        }
    }

    /* ---- Memory map ---- */
    const struct limine_memmap_response *mm = memmap_request.response;
    if (mm != NULL) {
        if (mm->entry_count == 0 || mm->entries == NULL) {
            boot_fail(bi, BOOT_CHK_MEMMAP, "map is empty");
        } else {
            bi->region_total = (size_t)mm->entry_count;
            for (u64 i = 0; i < mm->entry_count; i++) {
                const struct limine_memmap_entry *e = mm->entries[i];
                if (e == NULL) {
                    continue;
                }
                if (bi->region_count < MEM_MAX_REGIONS) {
                    bi->regions[bi->region_count].base   = e->base;
                    bi->regions[bi->region_count].length = e->length;
                    bi->regions[bi->region_count].type   =
                        translate_memmap_type(e->type);
                    bi->region_count++;
                }
            }
            if (bi->region_count == 0) {
                boot_fail(bi, BOOT_CHK_MEMMAP, "all entries were NULL");
            } else if (bi->region_count < bi->region_total) {
                boot_pass(bi, BOOT_CHK_MEMMAP,
                          "copied, but TRUNCATED (raise MEM_MAX_REGIONS)");
            } else {
                boot_pass(bi, BOOT_CHK_MEMMAP, "copied to kernel memory");
            }
        }
    }

    /* ---- Kernel address ---- */
    const struct limine_executable_address_response *ka =
        kernel_address_request.response;
    if (ka != NULL) {
        if (ka->virtual_base < KERNEL_IMAGE_START) {
            boot_fail(bi, BOOT_CHK_KADDR, "virtual base below the -2 GiB window");
        } else if ((ka->physical_base & (PAGE_SIZE - 1)) != 0) {
            boot_fail(bi, BOOT_CHK_KADDR, "physical base not page aligned");
        } else {
            bi->kernel_phys_base = ka->physical_base;
            bi->kernel_virt_base = ka->virtual_base;
            boot_pass(bi, BOOT_CHK_KADDR, "kernel placement known");
        }
    }

    /* ---- Framebuffer (optional) ---- */
    const struct limine_framebuffer_response *fbr = framebuffer_request.response;
    if (fbr != NULL) {
        if (fbr->framebuffer_count == 0 || fbr->framebuffers == NULL ||
            fbr->framebuffers[0] == NULL) {
            boot_fail(bi, BOOT_CHK_FRAMEBUFFER, "no framebuffer provided");
        } else {
            const struct limine_framebuffer *fb = fbr->framebuffers[0];
            if (fb->memory_model != LIMINE_FRAMEBUFFER_RGB) {
                boot_fail(bi, BOOT_CHK_FRAMEBUFFER, "not an RGB memory model");
            } else {
                bi->fb.address     = fb->address;
                bi->fb.width       = fb->width;
                bi->fb.height      = fb->height;
                bi->fb.pitch       = fb->pitch;
                bi->fb.bpp         = fb->bpp;
                bi->fb.red_size    = fb->red_mask_size;
                bi->fb.red_shift   = fb->red_mask_shift;
                bi->fb.green_size  = fb->green_mask_size;
                bi->fb.green_shift = fb->green_mask_shift;
                bi->fb.blue_size   = fb->blue_mask_size;
                bi->fb.blue_shift  = fb->blue_mask_shift;
                boot_pass(bi, BOOT_CHK_FRAMEBUFFER, "graphical output available");
            }
        }
    }
}

void boot_report_and_enforce(const boot_info_t *bi) {
    const boot_check_t *first_fatal = NULL;

    for (size_t i = 0; i < BOOT_CHK_COUNT; i++) {
        const boot_check_t *c = &bi->checks[i];

        if (c->ok) {
            kok("%-22s %s", c->name, c->detail);
        } else if (c->fatal) {
            kerror("%-22s %s", c->name, c->detail);
            if (first_fatal == NULL) {
                first_fatal = c;
            }
        } else {
            kwarn("%-22s %s (continuing without)", c->name, c->detail);
        }
    }

    if (bi->checks[BOOT_CHK_HHDM].ok) {
        kinfo("HHDM offset   : 0x%016lx", bi->hhdm_offset);
    }
    if (bi->checks[BOOT_CHK_KADDR].ok) {
        kinfo("Kernel phys   : 0x%016lx", bi->kernel_phys_base);
        kinfo("Kernel virt   : 0x%016lx", bi->kernel_virt_base);
    }
    if (bi->checks[BOOT_CHK_FRAMEBUFFER].ok) {
        kinfo("Framebuffer   : %lux%lu, %u bpp, pitch %lu, at %p",
              bi->fb.width, bi->fb.height, (unsigned)bi->fb.bpp, bi->fb.pitch,
              bi->fb.address);
    }

    if (first_fatal != NULL) {
        kpanic("Required boot protocol feature unavailable: %s (%s)",
               first_fatal->name, first_fatal->detail);
    }
}

static bool in_region(u64 addr, const mem_region_t *r) {
    return addr >= r->base && addr < r->base + r->length;
}

void boot_reclaim_memory(void) {
    u64 rsp;
    __asm__ volatile ("movq %%rsp, %0" : "=r"(rsp));
    u64 cr3 = read_cr3() & 0x000FFFFFFFFFF000ULL;
    bool rsp_in_hhdm = (rsp - g_boot.hhdm_offset) < (512ULL * GiB);
    u64 rsp_phys = rsp_in_hhdm ? rsp - g_boot.hhdm_offset : 0;

    /* Guard rails: refuse if we are still standing on what we are freeing. */
    for (size_t i = 0; i < g_boot.region_count; i++) {
        const mem_region_t *r = &g_boot.regions[i];
        if (r->type != MEMT_BOOTLOADER_RECLAIMABLE) {
            continue;
        }
        if (in_region(cr3, r)) {
            kpanic("reclaim: still on the bootloader's page tables (CR3=0x%lx)", cr3);
        }
        if (rsp_in_hhdm && in_region(rsp_phys, r)) {
            kpanic("reclaim: still on the bootloader's stack (RSP=0x%lx)", rsp);
        }
    }

    u64 frames = 0;
    for (size_t i = 0; i < g_boot.region_count; i++) {
        const mem_region_t *r = &g_boot.regions[i];
        if (r->type == MEMT_BOOTLOADER_RECLAIMABLE) {
            frames += pmm_reclaim_range(r->base, r->length);
        }
    }
    kok("Reclaimed %lu KiB of bootloader memory", frames * (PAGE_SIZE / KiB));
}
