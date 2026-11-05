/*
 * MyOS - src/mm/memmap.c
 * Part of the MyOS 0.2.0 kernel (split out of the monolithic main.c).
 */

/* #########################################################################
   MEMORY MAP ANALYSIS & PMM PREPARATION
   -------------------------------------------------------------------------
   Pure analysis: nothing here mutates memory. The PMM (next milestone) will
   consume mem_info_t / pmm_plan_t.

   Policy captured in code and comments:
     USABLE                    -> frames the PMM may hand out.
     BOOTLOADER_RECLAIMABLE    -> NOT free yet. It holds the stack and page
                                  tables we are still running on. Reclaim only
                                  after switching to our own CR3 and stack.
     ACPI_RECLAIMABLE          -> keep until ACPI tables are parsed.
     everything else           -> never allocate.
   Memory below 1 MiB is "conventional": left out of the bitmap placement
   choice because it is scarce and wanted for DMA/SMP trampoline later.
   ######################################################################### */

#include "kernel.h"

const char *mem_type_name(mem_type_t t) {
    switch (t) {
    case MEMT_USABLE:                 return "USABLE";
    case MEMT_RESERVED:               return "RESERVED";
    case MEMT_ACPI_RECLAIMABLE:       return "ACPI_RECLAIMABLE";
    case MEMT_ACPI_NVS:               return "ACPI_NVS";
    case MEMT_BAD:                    return "BAD_MEMORY";
    case MEMT_BOOTLOADER_RECLAIMABLE: return "BOOTLOADER_RECLAIMABLE";
    case MEMT_KERNEL_AND_MODULES:     return "KERNEL_AND_MODULES";
    case MEMT_FRAMEBUFFER:            return "FRAMEBUFFER";
    default:                          return "UNKNOWN";
    }
}

static u64 range_overlap(u64 b, u64 e, u64 lo, u64 hi) {
    u64 s = (b > lo) ? b : lo;
    u64 t = (e < hi) ? e : hi;
    return (t > s) ? t - s : 0;
}

void mem_init(const boot_info_t *bi, mem_info_t *mi) {
    memset(mi, 0, sizeof *mi);
    mi->regions   = bi->regions;
    mi->count     = bi->region_count;
    mi->truncated = bi->region_count < bi->region_total;

    u64 prev_end = 0;

    for (size_t i = 0; i < mi->count; i++) {
        const mem_region_t *r = &mi->regions[i];
        u64 end = r->base + r->length;

        if (end < r->base) {            /* 64-bit overflow */
            mi->anomalies++;
            continue;
        }
        if (r->base < prev_end) {       /* protocol promises sorted, disjoint */
            mi->anomalies++;
        }
        prev_end = end;

        mi->bytes_by_type[r->type] += r->length;

        if (r->type != MEMT_USABLE) {
            continue;
        }

        mi->usable_conventional += range_overlap(r->base, end, 0, 1 * MiB);
        mi->usable_low          += range_overlap(r->base, end, 1 * MiB, 4 * GiB);
        mi->usable_high         += range_overlap(r->base, end, 4 * GiB, UINT64_MAX);

        u64 a_base = ALIGN_UP(r->base, PAGE_SIZE);
        u64 a_end  = ALIGN_DOWN(end, PAGE_SIZE);
        if (a_end > a_base) {
            mi->usable_frames += (a_end - a_base) / PAGE_SIZE;
            if (a_end > mi->highest_usable_end) {
                mi->highest_usable_end = a_end;
            }
        }
    }
}

void mem_report(const mem_info_t *mi) {
    char size[32];

    kinfo("---------------- MEMORY MAP (%lu regions) ----------------",
          (u64)mi->count);

    for (size_t i = 0; i < mi->count; i++) {
        const mem_region_t *r = &mi->regions[i];
        log_level_t lvl = LOG_INFO;

        if (r->type == MEMT_USABLE) lvl = LOG_OK;
        else if (r->type == MEMT_BAD) lvl = LOG_WARN;

        fmt_size(size, sizeof size, r->length);
        klog(lvl, "#%02zu 0x%016lx - 0x%016lx %10s  %s",
             i, r->base, r->base + r->length, size, mem_type_name(r->type));
    }

    kinfo("-------------------------- SUMMARY -----------------------");
    for (int t = 0; t < MEMT_COUNT; t++) {
        if (mi->bytes_by_type[t] == 0) {
            continue;
        }
        fmt_size(size, sizeof size, mi->bytes_by_type[t]);
        kinfo("%-24s %s", mem_type_name((mem_type_t)t), size);
    }

    kok("Usable RAM        : %lu bytes (%lu MiB)",
        mi->bytes_by_type[MEMT_USABLE],
        mi->bytes_by_type[MEMT_USABLE] / MiB);
    kinfo("  conventional (<1 MiB)   : %lu KiB", mi->usable_conventional / KiB);
    kinfo("  low  (1 MiB .. 4 GiB)   : %lu MiB", mi->usable_low / MiB);
    kinfo("  high (>= 4 GiB)         : %lu MiB", mi->usable_high / MiB);
    kinfo("Usable 4 KiB frames : %lu", mi->usable_frames);
    kinfo("Highest usable addr : 0x%lx", mi->highest_usable_end);

    if (mi->bytes_by_type[MEMT_BOOTLOADER_RECLAIMABLE] != 0) {
        fmt_size(size, sizeof size, mi->bytes_by_type[MEMT_BOOTLOADER_RECLAIMABLE]);
        kwarn("%s of bootloader-reclaimable memory is RESERVED until we leave "
              "Limine's stack and page tables", size);
    }
    if (mi->truncated) {
        kwarn("Memory map truncated: only %lu regions kept", (u64)mi->count);
    }
    if (mi->anomalies != 0) {
        kwarn("Memory map had %u ordering/overflow anomalies", mi->anomalies);
    }
    if (mi->bytes_by_type[MEMT_USABLE] == 0) {
        kpanic("No usable memory reported by the bootloader");
    }
}