/*
 * MyOS - drivers/ahci.c
 * Polled AHCI (SATA) driver: ATA READ/WRITE DMA EXT on command slot 0.
 */

#include "drivers/ahci.h"
#include "kernel/printk.h"
#include "kernel/error.h"
#include "mm/dma.h"
#include "mm/vmm.h"
#include "drivers/pci.h"

#define MAX_AHCI_PORTS   32

/* Register bits */
#define HBA_CAP_S64A     (1u << 31)
#define HBA_GHC_IE       (1u << 1)
#define HBA_GHC_AE       (1u << 31)

#define PORT_CMD_ST      (1u << 0)
#define PORT_CMD_FRE     (1u << 4)
#define PORT_CMD_FR      (1u << 14)
#define PORT_CMD_CR      (1u << 15)
#define PORT_IS_TFES     (1u << 30)
#define PORT_TFD_ERR     (1u << 0)
#define PORT_TFD_DRQ     (1u << 3)
#define PORT_TFD_BSY     (1u << 7)

#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35

/* Limits */
#define AHCI_PAGE            4096u
#define AHCI_PRDT_MAX        32u
#define AHCI_PRD_MAX_BYTES   (4u * 1024u * 1024u)
#define AHCI_CHUNK_SECTORS   128u
#define AHCI_LBA_LIMIT       (1ULL << 48)
#define AHCI_POLL_SPINS      200000000UL

static ahci_device_t ahci_devices[MAX_AHCI_PORTS];
static hba_mem_t    *hba_mem  = NULL;
static int           g_s64a   = 0;

static inline void ahci_relax(void)
{
    __asm__ volatile ("pause" ::: "memory");
}

static inline void ahci_fence(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* Spin until (*reg & mask) == 0, bounded. */
static int ahci_wait_clear(volatile uint32_t *reg, uint32_t mask)
{
    unsigned long spins = AHCI_POLL_SPINS;

    while (*reg & mask) {
        if (spins-- == 0)
            return -E_TIMEDOUT;

        ahci_relax();
    }

    return 0;
}

/* ---- command engine control (AHCI 1.3.1 section 10.3) --------------------- */

static int ahci_stop_port(hba_port_t *port)
{
    port->cmd &= ~PORT_CMD_ST;

    if (ahci_wait_clear(&port->cmd, PORT_CMD_CR))
        return -E_TIMEDOUT;

    if (port->cmd & PORT_CMD_FRE) {
        port->cmd &= ~PORT_CMD_FRE;

        if (ahci_wait_clear(&port->cmd, PORT_CMD_FR))
            return -E_TIMEDOUT;
    }

    return 0;
}

static int ahci_start_port(hba_port_t *port)
{
    if (ahci_wait_clear(&port->cmd, PORT_CMD_CR))
        return -E_TIMEDOUT;

    port->cmd |= PORT_CMD_FRE;

    if (ahci_wait_clear(&port->tfd, PORT_TFD_BSY | PORT_TFD_DRQ))
        return -E_TIMEDOUT;

    port->cmd |= PORT_CMD_ST;

    return 0;
}

static int ahci_port_recover(hba_port_t *port)
{
    int rc = ahci_stop_port(port);

    port->serr = 0xFFFFFFFFu;
    port->is   = 0xFFFFFFFFu;

    if (rc)
        return rc;

    return ahci_start_port(port);
}

static int ahci_check_port_type(hba_port_t *port)
{
    uint32_t ssts = port->ssts;
    uint8_t ipm = (ssts >> 8) & 0x0F;
    uint8_t det = ssts & 0x0F;

    if (det != 3)
        return AHCI_DEV_NULL;

    if (ipm != 1)
        return AHCI_DEV_NULL;

    switch (port->sig) {
        case 0x00000101: return AHCI_DEV_SATA;
        case 0xEB140101: return AHCI_DEV_SATAPI;
        case 0xC33C0101: return AHCI_DEV_SEMB;
        case 0x96690101: return AHCI_DEV_PM;
        default:         return AHCI_DEV_NULL;
    }
}

/* ---- DMA allocation helpers using dma.h ---------------------------------- */

static void *ahci_dma_alloc_buf(size_t size, size_t align,
                                uint64_t *phys, dma_buf_t *buf)
{
    u64 mask = g_s64a ? DMA_MASK_64 : DMA_MASK_32;

    int32_t err = dma_alloc(buf, size, align, mask, DMA_ZERO);

    if (err < 0)
        return NULL;

    *phys = buf->phys;

    return buf->virt;
}

static void ahci_port_free(ahci_device_t *d)
{
    if (d->cl_virt) dma_free(&d->cl_buf);
    if (d->fb_virt) dma_free(&d->fb_buf);
    if (d->ct_virt) dma_free(&d->ct_buf);

    d->cl_virt = NULL;
    d->fb_virt = NULL;
    d->ct_virt = NULL;

    d->implemented = 0;
}

static int ahci_port_setup(ahci_device_t *d, hba_port_t *port)
{
    d->ct_size = offsetof(hba_cmd_tbl_t, prdt_entry) +
                 AHCI_PRDT_MAX * sizeof(hba_prdt_entry_t);

    d->cl_virt = ahci_dma_alloc_buf(
        1024, 1024, &d->cl_phys, &d->cl_buf);

    d->fb_virt = ahci_dma_alloc_buf(
        256, 256, &d->fb_phys, &d->fb_buf);

    d->ct_virt = ahci_dma_alloc_buf(
        d->ct_size, 128, &d->ct_phys, &d->ct_buf);

    if (!d->cl_virt || !d->fb_virt || !d->ct_virt) {
        ahci_port_free(d);
        return -E_NOMEM;
    }

    port->clb  = (uint32_t)(d->cl_phys & 0xFFFFFFFFu);
    port->clbu = (uint32_t)(d->cl_phys >> 32);

    port->fb  = (uint32_t)(d->fb_phys & 0xFFFFFFFFu);
    port->fbu = (uint32_t)(d->fb_phys >> 32);

    hba_cmd_header_t *hdr = (hba_cmd_header_t *)d->cl_virt;

    hdr[0].ctba  = (uint32_t)(d->ct_phys & 0xFFFFFFFFu);
    hdr[0].ctbau = (uint32_t)(d->ct_phys >> 32);

    port->serr = 0xFFFFFFFFu;
    port->is   = 0xFFFFFFFFu;
    port->ie   = 0;

    ahci_fence();

    int rc = ahci_start_port(port);

    if (rc) {
        ahci_port_free(d);
        return rc;
    }

    return 0;
}

/* ---- init ----------------------------------------------------------------- */

int ahci_init(pci_device_t *pci_dev)
{
    if (!pci_dev)
        return -E_NODEV;

    /* Enable Bus Mastering and Memory Space decoding via PCI API */
    pci_enable_device(pci_dev, PCI_CMD_MEM | PCI_CMD_MASTER);
    pci_set_bus_master(pci_dev, true);

    /* Fetch BAR 5 using pci_get_bar */
    pci_bar_t bar5;

    if (pci_get_bar(pci_dev, 5, &bar5) < 0 ||
        bar5.type == PCI_BAR_NONE) {
        return -E_NODEV;
    }

    uint64_t abar_phys = bar5.base;

    if (abar_phys == 0)
        return -E_NODEV;

    /* Map the AHCI HBA register block into virtual memory */
    hba_mem = (hba_mem_t *)vmm_map_mmio(abar_phys, sizeof(hba_mem_t));

    if (!hba_mem)
        return -E_NOMEM;

    g_s64a = (hba_mem->cap & HBA_CAP_S64A) != 0;

    hba_mem->ghc = (hba_mem->ghc | HBA_GHC_AE) & ~HBA_GHC_IE;
    hba_mem->is  = 0xFFFFFFFFu;

    int usable = 0;
    uint32_t pi = hba_mem->pi;

    for (int i = 0; i < MAX_AHCI_PORTS; i++) {
        ahci_device_t *d = &ahci_devices[i];

        d->port_no     = i;
        d->implemented = 0;
        d->type        = AHCI_DEV_NULL;
        d->lock        = 0;

        if (!(pi & (1u << i)))
            continue;

        hba_port_t *port = &hba_mem->ports[i];

        if (ahci_stop_port(port)) {
            kwarn("AHCI: port %d would not stop, skipping", i);
            continue;
        }

        int dt = ahci_check_port_type(port);

        d->type = dt;

        if (dt == AHCI_DEV_NULL)
            continue;

        d->hba_port = port;

        if (dt != AHCI_DEV_SATA) {
            kinfo("AHCI: port %d has unsupported device type %d", i, dt);
            continue;
        }

        int rc = ahci_port_setup(d, port);

        if (rc) {
            kwarn("AHCI: port %d setup failed (%d)", i, rc);
            continue;
        }

        d->implemented = 1;
        usable++;

        kinfo("AHCI: port %d ready (SATA)", i);
    }

    return usable;
}

int ahci_first_sata_port(void)
{
    for (int i = 0; i < MAX_AHCI_PORTS; i++) {
        if (ahci_devices[i].implemented &&
            ahci_devices[i].type == AHCI_DEV_SATA) {
            return i;
        }
    }

    return -1;
}

/* ---- transfers ------------------------------------------------------------ */

static void ahci_lock(ahci_device_t *d)
{
    while (__atomic_exchange_n(&d->lock, 1, __ATOMIC_ACQUIRE))
        ahci_relax();
}

static void ahci_unlock(ahci_device_t *d)
{
    __atomic_store_n(&d->lock, 0, __ATOMIC_RELEASE);
}

static int ahci_set_prd(hba_cmd_tbl_t *tbl, unsigned idx,
                        uint64_t pa, size_t len)
{
    if (idx >= AHCI_PRDT_MAX)
        return -E_INVAL;

    if (!g_s64a && (pa + len) > 0x100000000ULL)
        return -E_INVAL;

    tbl->prdt_entry[idx].dba  = (uint32_t)(pa & 0xFFFFFFFFu);
    tbl->prdt_entry[idx].dbau = (uint32_t)(pa >> 32);
    tbl->prdt_entry[idx].rsv0 = 0;
    tbl->prdt_entry[idx].dbc  = (uint32_t)(len - 1);
    tbl->prdt_entry[idx].rsv1 = 0;
    tbl->prdt_entry[idx].i    = 0;

    return 0;
}

static int ahci_build_prdt(hba_cmd_tbl_t *tbl,
                           uint8_t *buf, size_t bytes)
{
    uintptr_t va = (uintptr_t)buf;
    uint64_t seg_pa = 0;
    size_t seg_len = 0;
    unsigned n = 0;
    int rc;

    while (bytes) {
        size_t in_page = AHCI_PAGE - (va & (AHCI_PAGE - 1));
        size_t len = in_page < bytes ? in_page : bytes;

        uint64_t pa = virt_to_phys_hhdm((void *)va);

        if (seg_len &&
            pa == seg_pa + seg_len &&
            seg_len + len <= AHCI_PRD_MAX_BYTES) {
            seg_len += len;
        } else {
            if (seg_len) {
                rc = ahci_set_prd(tbl, n++, seg_pa, seg_len);

                if (rc)
                    return rc;
            }

            seg_pa = pa;
            seg_len = len;
        }

        va += len;
        bytes -= len;
    }

    if (seg_len) {
        rc = ahci_set_prd(tbl, n++, seg_pa, seg_len);

        if (rc)
            return rc;
    }

    return (int)n;
}

static int ahci_do_chunk(ahci_device_t *d, uint64_t lba,
                         uint32_t count, uint8_t *buf, int is_write)
{
    hba_port_t *port = d->hba_port;
    hba_cmd_header_t *hdr = (hba_cmd_header_t *)d->cl_virt;
    hba_cmd_tbl_t *tbl = d->ct_virt;

    const uint32_t slot_mask = 1u;

    if (ahci_wait_clear(&port->tfd,
                        PORT_TFD_BSY | PORT_TFD_DRQ)) {
        ahci_port_recover(port);
        return -E_TIMEDOUT;
    }

    memset(tbl, 0, d->ct_size);

    int nprd = ahci_build_prdt(
        tbl, buf, (size_t)count * AHCI_SECTOR_SIZE);

    if (nprd < 0)
        return nprd;

    uint32_t ctba  = hdr[0].ctba;
    uint32_t ctbau = hdr[0].ctbau;

    memset(&hdr[0], 0, sizeof hdr[0]);

    hdr[0].cmd_fis_length =
        sizeof(struct reg_fis_h2d) / sizeof(uint32_t);

    hdr[0].write       = is_write ? 1 : 0;
    hdr[0].prdt_length = (uint16_t)nprd;
    hdr[0].ctba        = ctba;
    hdr[0].ctbau       = ctbau;

    struct reg_fis_h2d *fis =
        (struct reg_fis_h2d *)tbl->cfis;

    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c        = 1;

    fis->command = is_write
        ? ATA_CMD_WRITE_DMA_EXT
        : ATA_CMD_READ_DMA_EXT;

    fis->device = 1 << 6;

    fis->lba0 = (uint8_t)(lba);
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);

    fis->countl = (uint8_t)(count & 0xFF);
    fis->counth = (uint8_t)((count >> 8) & 0xFF);

    port->is   = 0xFFFFFFFFu;
    port->serr = 0xFFFFFFFFu;

    ahci_fence();

    port->ci = slot_mask;

    unsigned long spins = AHCI_POLL_SPINS;

    while (port->ci & slot_mask) {
        if (port->is & PORT_IS_TFES)
            break;

        if (spins-- == 0) {
            if (ahci_port_recover(port)) {
                kerror("AHCI: port %d hung, disabling", d->port_no);
                d->implemented = 0;
            }

            return -E_TIMEDOUT;
        }

        ahci_relax();
    }

    ahci_fence();

    if ((port->is & PORT_IS_TFES) ||
        (port->tfd & PORT_TFD_ERR)) {
        if (ahci_port_recover(port)) {
            kerror("AHCI: port %d failed to recover, disabling",
                   d->port_no);

            d->implemented = 0;
        }

        return -E_IO;
    }

    return 0;
}

static int ahci_transfer(int port_no, uint64_t lba,
                         uint32_t count, void *buf, int is_write)
{
    if (port_no < 0 || port_no >= MAX_AHCI_PORTS)
        return -E_NODEV;

    ahci_device_t *d = &ahci_devices[port_no];

    if (!d->implemented || d->type != AHCI_DEV_SATA)
        return -E_NODEV;

    if (!buf || count == 0)
        return -E_INVAL;

    if (((uintptr_t)buf & 1) != 0)
        return -E_INVAL;

    if (lba >= AHCI_LBA_LIMIT ||
        count > AHCI_LBA_LIMIT - lba) {
        return -E_INVAL;
    }

    uint8_t *p = (uint8_t *)buf;
    int rc = 0;

    ahci_lock(d);

    while (count && !rc) {
        uint32_t n = count > AHCI_CHUNK_SECTORS
            ? AHCI_CHUNK_SECTORS
            : count;

        rc = ahci_do_chunk(d, lba, n, p, is_write);

        lba   += n;
        count -= n;
        p     += (size_t)n * AHCI_SECTOR_SIZE;
    }

    ahci_unlock(d);

    return rc;
}

int ahci_read(int port_no, uint64_t lba,
              uint32_t sector_count, void *buf)
{
    return ahci_transfer(port_no, lba, sector_count, buf, 0);
}

int ahci_write(int port_no, uint64_t lba,
               uint32_t sector_count, const void *buf)
{
    return ahci_transfer(
        port_no, lba, sector_count, (void *)buf, 1);
}