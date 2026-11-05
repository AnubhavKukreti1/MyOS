/*
 * MyOS - src/drivers/pci.c
 * PCI/PCIe enumeration. Enumeration runs once, single-threaded, at boot and
 * the device table is immutable afterwards, so lookups need no locking.
 * Mechanism #1 needs a two-step address/data sequence and is serialised.
 */

#include "kernel.h"
#include "kernel/error.h"
#include "kernel/spinlock.h"
#include "kernel/timeout.h"
#include "mm/vmm.h"
#include "drivers/pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8u
#define PCI_CONFIG_DATA    0xCFCu
#define PCI_ENABLE_BIT     0x80000000u

typedef struct {
    u16          seg;
    u8           start_bus, end_bus;
    u64          phys;
    volatile u8 *virt;
} ecam_window_t;

static struct {
    spinlock_t    lock;
    bool          initialised;
    bool          mech1;
    ecam_window_t ecam[PCI_MAX_ECAM];
    u32           ecam_count;
    pci_device_t  devs[PCI_MAX_DEVICES];
    u32           dev_count;
    bool          overflow_warned;
} g_pci;

/* =========================================================================
   LOW-LEVEL CONFIG ACCESS
   ========================================================================= */

/* Pointer to the start of a function's 4 KiB ECAM block, or NULL. */
static volatile u8 *ecam_ptr(const pci_addr_t *a) {
    for (u32 i = 0; i < g_pci.ecam_count; i++) {
        const ecam_window_t *w = &g_pci.ecam[i];

        if (w->seg == a->seg && a->bus >= w->start_bus && a->bus <= w->end_bus) {
            u64 o = ((u64)(a->bus - w->start_bus) << 20) |
                    ((u64)a->dev << 15) | ((u64)a->fn << 12);
            return w->virt + o;
        }
    }
    return NULL;
}

static void ecam_xfer(volatile u8 *p, u8 width, u32 *val, bool write) {
    if (write) {
        switch (width) {
        case 1:  *p = (u8)*val; break;
        case 2:  *(volatile u16 *)p = (u16)*val; break;
        default: *(volatile u32 *)p = *val; break;
        }
    } else {
        switch (width) {
        case 1:  *val = *p; break;
        case 2:  *val = *(volatile u16 *)p; break;
        default: *val = *(volatile u32 *)p; break;
        }
    }
}

static int32_t mech1_xfer(const pci_addr_t *a, u16 off, u8 width, u32 *val, bool write) {
    if (a->seg != 0 || (u32)off + width > 256u) {
        return -E_RANGE;
    }
    u32 addr = PCI_ENABLE_BIT | ((u32)a->bus << 16) | ((u32)a->dev << 11) |
               ((u32)a->fn << 8) | ((u32)off & 0xFCu);
    u16 port = (u16)(PCI_CONFIG_DATA + (off & 3u));

    irq_flags_t fl = spin_lock_irqsave(&g_pci.lock);
    outl((u16)PCI_CONFIG_ADDRESS, addr);
    if (write) {
        switch (width) {
        case 1:  outb(port, (u8)*val); break;
        case 2:  outw(port, (u16)*val); break;
        default: outl(port, *val); break;
        }
    } else {
        switch (width) {
        case 1:  *val = inb(port); break;
        case 2:  *val = inw(port); break;
        default: *val = inl(port); break;
        }
    }
    spin_unlock_irqrestore(&g_pci.lock, fl);
    return 0;
}

static int32_t cfg_xfer(const pci_addr_t *a, u16 off, u8 width, u32 *val, bool write) {
    if (a == NULL || val == NULL || a->dev >= 32 || a->fn >= 8) {
        return -E_INVAL;
    }
    if (width != 1 && width != 2 && width != 4) {
        return -E_INVAL;
    }
    if ((off & (width - 1u)) != 0) {
        return -E_INVAL;
    }

    volatile u8 *e = ecam_ptr(a);
    if (e != NULL) {
        if ((u32)off + width > 4096u) {
            return -E_RANGE;
        }
        ecam_xfer(e + off, width, val, write);
        return 0;
    }
    if (g_pci.mech1 && a->seg == 0) {
        return mech1_xfer(a, off, width, val, write);
    }
    return -E_NODEV;
}

int32_t pci_addr_read(const pci_addr_t *a, u16 off, u8 width, u32 *val) {
    return cfg_xfer(a, off, width, val, false);
}

int32_t pci_addr_write(const pci_addr_t *a, u16 off, u8 width, u32 val) {
    return cfg_xfer(a, off, width, &val, true);
}

int32_t pci_read_config_u8(const pci_device_t *d, u16 off, u8 *v) {
    u32 t;
    if (d == NULL || v == NULL) { return -E_INVAL; }
    int32_t rc = cfg_xfer(&d->addr, off, 1, &t, false);
    if (rc == 0) { *v = (u8)t; }
    return rc;
}

int32_t pci_read_config_u16(const pci_device_t *d, u16 off, u16 *v) {
    u32 t;
    if (d == NULL || v == NULL) { return -E_INVAL; }
    int32_t rc = cfg_xfer(&d->addr, off, 2, &t, false);
    if (rc == 0) { *v = (u16)t; }
    return rc;
}

int32_t pci_read_config_u32(const pci_device_t *d, u16 off, u32 *v) {
    if (d == NULL || v == NULL) { return -E_INVAL; }
    return cfg_xfer(&d->addr, off, 4, v, false);
}

int32_t pci_write_config_u8(const pci_device_t *d, u16 off, u8 v) {
    u32 t = v;
    if (d == NULL) { return -E_INVAL; }
    return cfg_xfer(&d->addr, off, 1, &t, true);
}

int32_t pci_write_config_u16(const pci_device_t *d, u16 off, u16 v) {
    u32 t = v;
    if (d == NULL) { return -E_INVAL; }
    return cfg_xfer(&d->addr, off, 2, &t, true);
}

int32_t pci_write_config_u32(const pci_device_t *d, u16 off, u32 v) {
    if (d == NULL) { return -E_INVAL; }
    return cfg_xfer(&d->addr, off, 4, &v, true);
}

/* =========================================================================
   ACPI: RSDP -> RSDT/XSDT -> MCFG  (ECAM discovery)
   ========================================================================= */

typedef struct PACKED {
    char sig[8];
    u8   checksum;
    char oem[6];
    u8   revision;
    u32  rsdt_addr;
    u32  length;
    u64  xsdt_addr;
    u8   ext_checksum;
    u8   reserved[3];
} acpi_rsdp_t;

typedef struct PACKED {
    char sig[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oem[6];
    char oem_table[8];
    u32  oem_revision;
    u32  creator;
    u32  creator_revision;
} acpi_sdt_t;

typedef struct PACKED {
    u64 base;
    u16 segment;
    u8  start_bus;
    u8  end_bus;
    u32 reserved;
} acpi_mcfg_entry_t;

static bool acpi_sum_ok(const void *p, size_t len) {
    const u8 *b = p;
    u8 sum = 0;

    for (size_t i = 0; i < len; i++) {
        sum = (u8)(sum + b[i]);
    }
    return sum == 0;
}

/* Maps and validates a whole SDT (checksum included). */
static const acpi_sdt_t *acpi_map_sdt(u64 phys) {
    const acpi_sdt_t *h = vmm_map_mmio(phys, sizeof(acpi_sdt_t));
    if (h == NULL) {
        return NULL;
    }
    u32 len = h->length;
    if (len < sizeof(acpi_sdt_t) || len > 16 * MiB) {
        return NULL;
    }
    const acpi_sdt_t *full = vmm_map_mmio(phys, len);
    if (full == NULL || !acpi_sum_ok(full, len)) {
        return NULL;
    }
    return full;
}

/* Returns the physical address of a valid RSDP inside [p, p+len), or 0. */
static u64 rsdp_scan(const u8 *p, size_t len, u64 phys_base) {
    for (size_t off = 0; off + sizeof(acpi_rsdp_t) <= len; off += 16) {
        if (memcmp(p + off, "RSD PTR ", 8) != 0) {
            continue;
        }
        if (!acpi_sum_ok(p + off, 20)) {
            continue;
        }
        const acpi_rsdp_t *r = (const acpi_rsdp_t *)(p + off);
        if (r->revision >= 2 && (r->length < sizeof(acpi_rsdp_t) ||
                                 off + r->length > len ||
                                 !acpi_sum_ok(p + off, r->length))) {
            continue;
        }
        return phys_base + off;
    }
    return 0;
}

static u64 acpi_find_rsdp_bios(void) {
    /* The EBDA segment lives at physical 0x40E; the RSDP is in the first KiB
     * of the EBDA or in 0xE0000..0xFFFFF. */
    const u8 *bda = vmm_map_mmio(0, PAGE_SIZE);
    if (bda != NULL) {
        u16 seg = *(const u16 *)(bda + 0x40E);
        u64 ebda = (u64)seg << 4;

        if (ebda >= 0x80000 && ebda < 0xA0000) {
            const u8 *e = vmm_map_mmio(ebda, 1024);
            if (e != NULL) {
                u64 r = rsdp_scan(e, 1024, ebda);
                if (r != 0) {
                    return r;
                }
            }
        }
    }
    const u8 *hi = vmm_map_mmio(0xE0000, 0x20000);
    return (hi != NULL) ? rsdp_scan(hi, 0x20000, 0xE0000) : 0;
}

int32_t pci_ecam_add(u16 seg, u8 start_bus, u8 end_bus, u64 phys) {
    if (end_bus < start_bus || (phys & (PAGE_SIZE - 1)) != 0) {
        return -E_INVAL;
    }
    if (g_pci.ecam_count >= PCI_MAX_ECAM) {
        return -E_NOSPC;
    }
    u64 size = (u64)(end_bus - start_bus + 1) << 20;
    void *v = vmm_map_mmio(phys, size);
    if (v == NULL) {
        return -E_NOMEM;
    }
    ecam_window_t *w = &g_pci.ecam[g_pci.ecam_count++];
    w->seg       = seg;
    w->start_bus = start_bus;
    w->end_bus   = end_bus;
    w->phys      = phys;
    w->virt      = v;
    return 0;
}

static int32_t pci_acpi_discover_ecam(u64 rsdp_phys) {
    if (rsdp_phys == 0) {
        rsdp_phys = acpi_find_rsdp_bios();
    }
    if (rsdp_phys == 0) {
        return -E_NOENT;
    }
    const acpi_rsdp_t *rsdp = vmm_map_mmio(rsdp_phys, sizeof(acpi_rsdp_t));
    if (rsdp == NULL) {
        return -E_NOMEM;
    }

    u64 root_phys;
    u32 esz;
    if (rsdp->revision >= 2 && rsdp->xsdt_addr != 0) {
        root_phys = rsdp->xsdt_addr;
        esz = 8;
    } else {
        root_phys = rsdp->rsdt_addr;
        esz = 4;
    }
    const acpi_sdt_t *root = acpi_map_sdt(root_phys);
    if (root == NULL) {
        return -E_PROTO;
    }

    u32 n = (root->length - (u32)sizeof(acpi_sdt_t)) / esz;
    if (n > 512) {
        n = 512;
    }
    const u8 *ent = (const u8 *)root + sizeof(acpi_sdt_t);
    int32_t found = -E_NOENT;

    for (u32 i = 0; i < n; i++) {
        u64 tphys = 0;
        memcpy(&tphys, ent + (size_t)i * esz, esz);     /* little endian */

        const acpi_sdt_t *t = acpi_map_sdt(tphys);
        if (t == NULL || memcmp(t->sig, "MCFG", 4) != 0) {
            continue;
        }
        u32 entries = (t->length - (u32)sizeof(acpi_sdt_t) - 8u) / (u32)sizeof(acpi_mcfg_entry_t);
        const u8 *base = (const u8 *)t + sizeof(acpi_sdt_t) + 8;

        for (u32 k = 0; k < entries; k++) {
            acpi_mcfg_entry_t e;
            memcpy(&e, base + (size_t)k * sizeof e, sizeof e);
            if (pci_ecam_add(e.segment, e.start_bus, e.end_bus, e.base) == 0) {
                kinfo("PCI: ECAM seg %u buses %u-%u at 0x%lx",
                      (unsigned)e.segment, (unsigned)e.start_bus,
                      (unsigned)e.end_bus, e.base);
                found = 0;
            }
        }
    }
    return found;
}

/* =========================================================================
   ENUMERATION
   ========================================================================= */

const char *pci_class_name(u8 c, u8 s) {
    switch (c) {
    case 0x00: return "Unclassified";
    case 0x01:
        switch (s) {
        case 0x00: return "SCSI controller";
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe controller";
        default:   return "Mass storage controller";
        }
    case 0x02: return "Network controller";
    case 0x03: return "Display controller";
    case 0x04: return "Multimedia controller";
    case 0x05: return "Memory controller";
    case 0x06:
        switch (s) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI-PCI bridge";
        default:   return "Bridge";
        }
    case 0x07: return "Communication controller";
    case 0x08: return "System peripheral";
    case 0x09: return "Input device";
    case 0x0C: return (s == 0x03) ? "USB controller" : "Serial bus controller";
    default:   return "Other";
    }
}

static void parse_caps(pci_device_t *d) {
    u16 status = 0;
    if (pci_read_config_u16(d, PCI_CFG_STATUS, &status) != 0 ||
        !(status & PCI_STATUS_CAP_LIST)) {
        return;
    }
    u8 ptr = 0;
    u16 start = (d->header_type == 2) ? 0x14 : PCI_CFG_CAP_PTR;
    if (pci_read_config_u8(d, start, &ptr) != 0) {
        return;
    }
    ptr &= 0xFC;

    for (u32 guard = 0; ptr >= 0x40 && guard < 48; guard++) {
        u8 id = 0, next = 0;
        u16 w = 0;
        u32 dw = 0;

        if (pci_read_config_u8(d, ptr, &id) != 0 ||
            pci_read_config_u8(d, (u16)(ptr + 1), &next) != 0) {
            return;
        }
        switch (id) {
        case PCI_CAP_PM:
            if (pci_read_config_u16(d, (u16)(ptr + 2), &w) == 0) {
                d->pm.offset      = ptr;
                d->pm.version     = (u8)(w & 7u);
                d->pm.d1          = (w >> 9) & 1u;
                d->pm.d2          = (w >> 10) & 1u;
                d->pm.pme_support = (u8)((w >> 11) & 0x1Fu);
            }
            break;
        case PCI_CAP_MSI:
            if (pci_read_config_u16(d, (u16)(ptr + 2), &w) == 0) {
                d->msi.offset           = ptr;
                d->msi.is64             = (w >> 7) & 1u;
                d->msi.per_vector_mask  = (w >> 8) & 1u;
                d->msi.max_vectors      = (u8)(1u << ((w >> 1) & 7u));
            }
            break;
        case PCI_CAP_MSIX:
            if (pci_read_config_u16(d, (u16)(ptr + 2), &w) == 0) {
                d->msix.offset     = ptr;
                d->msix.table_size = (u16)((w & 0x7FFu) + 1u);
                if (pci_read_config_u32(d, (u16)(ptr + 4), &dw) == 0) {
                    d->msix.table_bir    = (u8)(dw & 7u);
                    d->msix.table_offset = dw & ~7u;
                }
                if (pci_read_config_u32(d, (u16)(ptr + 8), &dw) == 0) {
                    d->msix.pba_bir    = (u8)(dw & 7u);
                    d->msix.pba_offset = dw & ~7u;
                }
            }
            break;
        case PCI_CAP_PCIE:
            if (pci_read_config_u16(d, (u16)(ptr + 2), &w) == 0) {
                d->pcie.offset    = ptr;
                d->pcie.version   = (u8)(w & 0xFu);
                d->pcie.port_type = (u8)((w >> 4) & 0xFu);
                if (pci_read_config_u32(d, (u16)(ptr + 0xC), &dw) == 0) {
                    d->pcie.max_link_speed = (u8)(dw & 0xFu);
                    d->pcie.max_link_width = (u8)((dw >> 4) & 0x3Fu);
                }
            }
            break;
        default:
            break;
        }
        ptr = next & 0xFC;
    }
}

static bool visited_test_set(u8 *bm, u8 bus) {
    bool was = (bm[bus >> 3] >> (bus & 7)) & 1u;
    bm[bus >> 3] |= (u8)(1u << (bus & 7));
    return was;
}

static void scan_bus(u16 seg, u8 bus, u8 *visited, u32 depth);

static void probe_function(u16 seg, u8 bus, u8 dev, u8 fn, u8 *visited, u32 depth) {
    pci_addr_t a = { seg, bus, dev, fn };
    u32 v = 0;

    if (cfg_xfer(&a, 0, 4, &v, false) != 0 || (v & 0xFFFFu) == 0xFFFFu) {
        return;
    }
    if (g_pci.dev_count >= PCI_MAX_DEVICES) {
        if (!g_pci.overflow_warned) {
            kwarn("PCI: device table full (%u), ignoring the rest", (unsigned)PCI_MAX_DEVICES);
            g_pci.overflow_warned = true;
        }
        return;
    }

    pci_device_t *d = &g_pci.devs[g_pci.dev_count];
    memset(d, 0, sizeof *d);
    d->addr      = a;
    d->vendor_id = (u16)(v & 0xFFFFu);
    d->device_id = (u16)(v >> 16);

    u32 cls = 0, ht = 0;
    cfg_xfer(&a, PCI_CFG_REVISION, 4, &cls, false);
    cfg_xfer(&a, PCI_CFG_HEADER_TYPE, 1, &ht, false);
    d->revision      = (u8)(cls & 0xFFu);
    d->prog_if       = (u8)((cls >> 8) & 0xFFu);
    d->subclass      = (u8)((cls >> 16) & 0xFFu);
    d->class_code    = (u8)(cls >> 24);
    d->multifunction = (ht & 0x80u) != 0;
    d->header_type   = (u8)(ht & 0x7Fu);
    d->is_bridge     = (d->header_type == 1);

    if (d->header_type == 0) {
        u32 t = 0;
        if (cfg_xfer(&a, PCI_CFG_SUBSYS_VID, 4, &t, false) == 0) {
            d->subsys_vendor = (u16)(t & 0xFFFFu);
            d->subsys_id     = (u16)(t >> 16);
        }
    }
    u32 irq = 0;
    if (cfg_xfer(&a, PCI_CFG_IRQ_LINE, 4, &irq, false) == 0) {
        d->irq_line = (u8)(irq & 0xFFu);
        d->irq_pin  = (u8)((irq >> 8) & 0xFFu);
    }
    parse_caps(d);
    g_pci.dev_count++;

    kinfo("PCI %02x:%02x.%u  %04x:%04x  %02x.%02x.%02x  %s%s%s%s",
          (unsigned)bus, (unsigned)dev, (unsigned)fn,
          (unsigned)d->vendor_id, (unsigned)d->device_id,
          (unsigned)d->class_code, (unsigned)d->subclass, (unsigned)d->prog_if,
          pci_class_name(d->class_code, d->subclass),
          d->msi.offset ? " [MSI]" : "", d->msix.offset ? " [MSI-X]" : "",
          d->pcie.offset ? " [PCIe]" : "");

    if (d->is_bridge && depth < 32) {
        u32 sec = 0;
        if (cfg_xfer(&a, PCI_CFG_SECONDARY, 1, &sec, false) == 0 && sec != 0) {
            d->secondary_bus = (u8)sec;
            if (!visited_test_set(visited, (u8)sec)) {
                scan_bus(seg, (u8)sec, visited, depth + 1);
            }
        }
    }
}

static void scan_bus(u16 seg, u8 bus, u8 *visited, u32 depth) {
    visited_test_set(visited, bus);

    for (u8 dev = 0; dev < 32; dev++) {
        pci_addr_t a = { seg, bus, dev, 0 };
        u32 id = 0, ht = 0;

        if (cfg_xfer(&a, 0, 4, &id, false) != 0 || (id & 0xFFFFu) == 0xFFFFu) {
            continue;
        }
        probe_function(seg, bus, dev, 0, visited, depth);
        cfg_xfer(&a, PCI_CFG_HEADER_TYPE, 1, &ht, false);
        if (ht & 0x80u) {
            for (u8 fn = 1; fn < 8; fn++) {
                probe_function(seg, bus, dev, fn, visited, depth);
            }
        }
    }
}

static void enumerate_segment(u16 seg) {
    u8 visited[32];
    memset(visited, 0, sizeof visited);

    if (seg == 0 && g_pci.mech1) {
        scan_bus(0, 0, visited, 0);

        /* Multi-function host bridge = several root buses (bus number = fn). */
        pci_addr_t host = { 0, 0, 0, 0 };
        u32 ht = 0;
        if (cfg_xfer(&host, PCI_CFG_HEADER_TYPE, 1, &ht, false) == 0 && (ht & 0x80u)) {
            for (u8 fn = 1; fn < 8; fn++) {
                pci_addr_t h = { 0, 0, 0, fn };
                u32 id = 0;
                if (cfg_xfer(&h, 0, 4, &id, false) == 0 && (id & 0xFFFFu) != 0xFFFFu &&
                    !visited_test_set(visited, fn)) {
                    scan_bus(0, fn, visited, 0);
                }
            }
        }
    }
    for (u32 i = 0; i < g_pci.ecam_count; i++) {
        const ecam_window_t *w = &g_pci.ecam[i];
        if (w->seg == seg && !visited_test_set(visited, w->start_bus)) {
            scan_bus(seg, w->start_bus, visited, 0);
        }
    }
}

static bool mech1_detect(void) {
    u32 saved = inl((u16)PCI_CONFIG_ADDRESS);

    outl((u16)PCI_CONFIG_ADDRESS, PCI_ENABLE_BIT);
    bool ok = (inl((u16)PCI_CONFIG_ADDRESS) == PCI_ENABLE_BIT);
    outl((u16)PCI_CONFIG_ADDRESS, saved);
    return ok;
}

int32_t pci_init(u64 rsdp_phys) {
    if (g_pci.initialised) {
        return 0;
    }
    spin_init(&g_pci.lock);
    g_pci.mech1 = mech1_detect();

    int32_t arc = pci_acpi_discover_ecam(rsdp_phys);
    if (arc != 0) {
        kinfo("PCI: no ECAM (%s)", kerr_str(arc));
    }

    /* A bogus MCFG must not be trusted: compare against mechanism #1. */
    if (g_pci.mech1 && g_pci.ecam_count != 0) {
        pci_addr_t a0 = { 0, 0, 0, 0 };
        volatile u8 *e = ecam_ptr(&a0);
        u32 mv = 0;

        if (e != NULL && mech1_xfer(&a0, 0, 4, &mv, false) == 0) {
            u32 ev = *(volatile u32 *)e;
            if (ev != mv) {
                kwarn("PCI: ECAM (0x%08x) disagrees with mechanism #1 (0x%08x): ignoring ECAM",
                      ev, mv);
                g_pci.ecam_count = 0;
            }
        }
    }

    if (!g_pci.mech1 && g_pci.ecam_count == 0) {
        kerror("PCI: no usable configuration mechanism");
        return -E_NODEV;
    }
    kinfo("PCI: mechanism #1 %s, ECAM %u window(s)",
          g_pci.mech1 ? "present" : "absent", (unsigned)g_pci.ecam_count);

    u16 segs[PCI_MAX_ECAM + 1];
    u32 nseg = 0;
    segs[nseg++] = 0;
    for (u32 i = 0; i < g_pci.ecam_count; i++) {
        bool seen = false;
        for (u32 k = 0; k < nseg; k++) {
            if (segs[k] == g_pci.ecam[i].seg) { seen = true; }
        }
        if (!seen) { segs[nseg++] = g_pci.ecam[i].seg; }
    }
    for (u32 k = 0; k < nseg; k++) {
        enumerate_segment(segs[k]);
    }

    g_pci.initialised = true;
    kok("PCI: %u function(s) found", (unsigned)g_pci.dev_count);
    return 0;
}

/* =========================================================================
   LOOKUP / DEVICE CONTROL
   ========================================================================= */

u32 pci_device_count(void) {
    return g_pci.dev_count;
}

pci_device_t *pci_get_device(u32 index) {
    return (index < g_pci.dev_count) ? &g_pci.devs[index] : NULL;
}

pci_device_t *pci_find_device(u16 vendor, u16 device, u32 index) {
    for (u32 i = 0; i < g_pci.dev_count; i++) {
        pci_device_t *d = &g_pci.devs[i];

        if ((vendor == PCI_ANY_ID || d->vendor_id == vendor) &&
            (device == PCI_ANY_ID || d->device_id == device)) {
            if (index-- == 0) {
                return d;
            }
        }
    }
    return NULL;
}

pci_device_t *pci_find_class(u8 class_code, u8 subclass, int16_t prog_if, u32 index) {
    for (u32 i = 0; i < g_pci.dev_count; i++) {
        pci_device_t *d = &g_pci.devs[i];

        if (d->class_code == class_code && d->subclass == subclass &&
            (prog_if < 0 || d->prog_if == (u8)prog_if)) {
            if (index-- == 0) {
                return d;
            }
        }
    }
    return NULL;
}

u16 pci_find_capability(const pci_device_t *d, u8 cap_id) {
    u16 status = 0;
    u8 ptr = 0;

    if (d == NULL || pci_read_config_u16(d, PCI_CFG_STATUS, &status) != 0 ||
        !(status & PCI_STATUS_CAP_LIST) ||
        pci_read_config_u8(d, (d->header_type == 2) ? 0x14 : PCI_CFG_CAP_PTR, &ptr) != 0) {
        return 0;
    }
    ptr &= 0xFC;
    for (u32 guard = 0; ptr >= 0x40 && guard < 48; guard++) {
        u8 id = 0, next = 0;
        if (pci_read_config_u8(d, ptr, &id) != 0 ||
            pci_read_config_u8(d, (u16)(ptr + 1), &next) != 0) {
            return 0;
        }
        if (id == cap_id) {
            return ptr;
        }
        ptr = next & 0xFC;
    }
    return 0;
}

u16 pci_find_ext_capability(const pci_device_t *d, u16 cap_id) {
    if (d == NULL || ecam_ptr(&d->addr) == NULL) {
        return 0;                                 /* needs ECAM for offsets >= 0x100 */
    }
    u16 off = 0x100;
    for (u32 guard = 0; guard < 96; guard++) {
        u32 hdr = 0;
        if (pci_read_config_u32(d, off, &hdr) != 0 || hdr == 0 || hdr == 0xFFFFFFFFu) {
            return 0;
        }
        if ((hdr & 0xFFFFu) == cap_id) {
            return off;
        }
        u16 next = (u16)((hdr >> 20) & 0xFFCu);
        if (next < 0x100) {
            return 0;
        }
        off = next;
    }
    return 0;
}

int32_t pci_claim(pci_device_t *d) {
    if (d == NULL) {
        return -E_INVAL;
    }
    if (__atomic_exchange_n(&d->claimed, 1u, __ATOMIC_ACQ_REL) != 0) {
        return -E_BUSY;
    }
    return 0;
}

int32_t pci_enable_device(pci_device_t *d, u16 cmd_bits) {
    u16 cmd = 0;
    int32_t rc = pci_read_config_u16(d, PCI_CFG_COMMAND, &cmd);

    if (rc != 0) {
        return rc;
    }
    return pci_write_config_u16(d, PCI_CFG_COMMAND, (u16)(cmd | cmd_bits));
}

int32_t pci_set_bus_master(pci_device_t *d, bool on) {
    u16 cmd = 0;
    int32_t rc = pci_read_config_u16(d, PCI_CFG_COMMAND, &cmd);

    if (rc != 0) {
        return rc;
    }
    cmd = on ? (u16)(cmd | PCI_CMD_MASTER) : (u16)(cmd & ~PCI_CMD_MASTER);
    return pci_write_config_u16(d, PCI_CFG_COMMAND, cmd);
}

int32_t pci_set_intx(pci_device_t *d, bool enable) {
    u16 cmd = 0;
    int32_t rc = pci_read_config_u16(d, PCI_CFG_COMMAND, &cmd);

    if (rc != 0) {
        return rc;
    }
    cmd = enable ? (u16)(cmd & ~PCI_CMD_INTX_OFF) : (u16)(cmd | PCI_CMD_INTX_OFF);
    return pci_write_config_u16(d, PCI_CFG_COMMAND, cmd);
}

int32_t pci_get_bar(pci_device_t *d, u8 index, pci_bar_t *out) {
    if (d == NULL || out == NULL) {
        return -E_INVAL;
    }
    memset(out, 0, sizeof *out);

    u8 max = (d->header_type == 0) ? 6 : (d->header_type == 1) ? 2 : 0;
    if (index >= max) {
        return -E_INVAL;
    }
    const u16 off = (u16)(PCI_CFG_BAR0 + 4u * index);
    u32 orig = 0;
    u16 cmd = 0;
    int32_t rc = pci_read_config_u32(d, off, &orig);

    if (rc == 0) {
        rc = pci_read_config_u16(d, PCI_CFG_COMMAND, &cmd);
    }
    if (rc != 0) {
        return rc;
    }

    const bool io     = (orig & 1u) != 0;
    const bool is64   = !io && ((orig >> 1) & 3u) == 2u;
    u32 orig_hi = 0, lo = 0, hi = 0;

    if (is64) {
        if (index + 1 >= max) {
            return -E_INVAL;
        }
        pci_read_config_u32(d, (u16)(off + 4), &orig_hi);
    }

    /* Sizing: decode off, write all ones, read the mask back, restore. */
    pci_write_config_u16(d, PCI_CFG_COMMAND, (u16)(cmd & ~(PCI_CMD_IO | PCI_CMD_MEM)));
    pci_write_config_u32(d, off, 0xFFFFFFFFu);
    pci_read_config_u32(d, off, &lo);
    pci_write_config_u32(d, off, orig);
    if (is64) {
        pci_write_config_u32(d, (u16)(off + 4), 0xFFFFFFFFu);
        pci_read_config_u32(d, (u16)(off + 4), &hi);
        pci_write_config_u32(d, (u16)(off + 4), orig_hi);
    }
    pci_write_config_u16(d, PCI_CFG_COMMAND, cmd);

    u64 mask = io ? (u64)(lo & 0xFFFFFFFCu) : (u64)(lo & 0xFFFFFFF0u);
    if (is64) {
        mask |= (u64)hi << 32;
    }
    if (mask == 0) {
        out->type = PCI_BAR_NONE;
        return 0;
    }
    out->size = mask & (~mask + 1);               /* lowest writable address bit */
    if (io) {
        out->type = PCI_BAR_IO;
        out->base = orig & 0xFFFFFFFCu;
    } else {
        out->type         = is64 ? PCI_BAR_MEM64 : PCI_BAR_MEM32;
        out->prefetchable = (orig & 8u) != 0;
        out->base         = (u64)(orig & 0xFFFFFFF0u) | (is64 ? ((u64)orig_hi << 32) : 0);
    }
    return 0;
}

int32_t pci_pm_set_d0(pci_device_t *d) {
    if (d == NULL) {
        return -E_INVAL;
    }
    if (d->pm.offset == 0) {
        return 0;
    }
    const u16 off = (u16)(d->pm.offset + 4);          /* PMCSR */
    u16 v = 0;
    int32_t rc = pci_read_config_u16(d, off, &v);

    if (rc != 0 || (v & 3u) == 0) {
        return rc;
    }
    u16 prev = (u16)(v & 3u);
    rc = pci_write_config_u16(d, off, (u16)(v & ~3u));
    kdelay_ms(prev == 3 ? 10 : 1);                   /* D3hot -> D0 recovery time */
    return rc;
}

int32_t pci_msi_enable(pci_device_t *d, u64 msg_addr, u16 msg_data) {
    if (d == NULL) {
        return -E_INVAL;
    }
    if (d->msi.offset == 0) {
        return -E_NOTSUP;
    }
    if (!d->msi.is64 && msg_addr > 0xFFFFFFFFULL) {
        return -E_RANGE;
    }
    const u16 base = d->msi.offset;
    u16 ctrl = 0;
    int32_t rc = pci_read_config_u16(d, (u16)(base + 2), &ctrl);

    if (rc != 0) {
        return rc;
    }
    pci_write_config_u32(d, (u16)(base + 4), (u32)msg_addr);
    if (d->msi.is64) {
        pci_write_config_u32(d, (u16)(base + 8), (u32)(msg_addr >> 32));
        pci_write_config_u16(d, (u16)(base + 12), msg_data);
        if (d->msi.per_vector_mask) {
            pci_write_config_u32(d, (u16)(base + 16), 0);
        }
    } else {
        pci_write_config_u16(d, (u16)(base + 8), msg_data);
        if (d->msi.per_vector_mask) {
            pci_write_config_u32(d, (u16)(base + 12), 0);
        }
    }
    ctrl = (u16)((ctrl & ~(7u << 4)) | 1u);           /* one vector, enable */
    rc = pci_write_config_u16(d, (u16)(base + 2), ctrl);
    pci_set_intx(d, false);
    return rc;
}

int32_t pci_msi_disable(pci_device_t *d) {
    if (d == NULL) {
        return -E_INVAL;
    }
    if (d->msi.offset == 0) {
        return -E_NOTSUP;
    }
    u16 ctrl = 0;
    int32_t rc = pci_read_config_u16(d, (u16)(d->msi.offset + 2), &ctrl);

    if (rc != 0) {
        return rc;
    }
    return pci_write_config_u16(d, (u16)(d->msi.offset + 2), (u16)(ctrl & ~1u));
}