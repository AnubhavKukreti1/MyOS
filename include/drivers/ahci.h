#ifndef DRIVERS_AHCI_H
#define DRIVERS_AHCI_H

#include <stdint.h>
#include <stddef.h>
#include "drivers/pci.h"
#include "mm/dma.h"

#define AHCI_DEV_NULL   0
#define AHCI_DEV_SATA   1
#define AHCI_DEV_SEMB   2
#define AHCI_DEV_PM     3
#define AHCI_DEV_SATAPI 4

#define AHCI_SECTOR_SIZE 512u

/* ---- FIS types ------------------------------------------------------------ */
typedef enum {
    FIS_TYPE_REG_H2D   = 0x27,  /* Register FIS - host to device            */
    FIS_TYPE_REG_D2H   = 0x34,  /* Register FIS - device to host            */
    FIS_TYPE_DMA_ACT   = 0x39,  /* DMA activate FIS - device to host        */
    FIS_TYPE_DMA_SETUP = 0x41,  /* DMA setup FIS - bidirectional            */
    FIS_TYPE_DATA      = 0x46,  /* Data FIS - bidirectional                 */
    FIS_TYPE_BIST      = 0x58,  /* BIST activate FIS - bidirectional        */
    FIS_TYPE_PIO_SETUP = 0x5F,  /* PIO setup FIS - device to host           */
    FIS_TYPE_DEV_BITS  = 0xA1   /* Set device bits FIS - device to host     */
} fis_type_t;

/* Register FIS - host to device (20 bytes). Was used by ahci.c but never
 * defined anywhere: the original did not compile. */
struct reg_fis_h2d {
    uint8_t fis_type;           /* FIS_TYPE_REG_H2D */
    uint8_t pmport:4;           /* port multiplier port */
    uint8_t rsv0:3;
    uint8_t c:1;                /* 1 = command, 0 = control */
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0, lba1, lba2;
    uint8_t device;
    uint8_t lba3, lba4, lba5;
    uint8_t featureh;
    uint8_t countl, counth;
    uint8_t icc;
    uint8_t control;
    uint8_t rsv1[4];
} __attribute__((packed));

_Static_assert(sizeof(struct reg_fis_h2d) == 20, "H2D FIS must be 20 bytes");

/* ---- HBA registers -------------------------------------------------------- */
typedef volatile struct tag_hba_port {
    uint32_t clb;       /* 0x00 command list base, 1 KiB aligned   */
    uint32_t clbu;      /* 0x04 command list base upper 32         */
    uint32_t fb;        /* 0x08 FIS base, 256-byte aligned         */
    uint32_t fbu;       /* 0x0C FIS base upper 32                  */
    uint32_t is;        /* 0x10 interrupt status                   */
    uint32_t ie;        /* 0x14 interrupt enable                   */
    uint32_t cmd;       /* 0x18 command and status                 */
    uint32_t rsv0;      /* 0x1C                                    */
    uint32_t tfd;       /* 0x20 task file data                     */
    uint32_t sig;       /* 0x24 signature                          */
    uint32_t ssts;      /* 0x28 SStatus                            */
    uint32_t sctl;      /* 0x2C SControl                           */
    uint32_t serr;      /* 0x30 SError                             */
    uint32_t sact;      /* 0x34 SActive                            */
    uint32_t ci;        /* 0x38 command issue                      */
    uint32_t sntf;      /* 0x3C SNotification                      */
    uint32_t fbs;       /* 0x40 FIS-based switching control        */
    uint32_t rsv1[11];  /* 0x44-0x6F                               */
    uint32_t vendor[4]; /* 0x70-0x7F                               */
} hba_port_t;

typedef volatile struct tag_hba_memory {
    uint32_t cap;       /* 0x00 host capabilities                  */
    uint32_t ghc;       /* 0x04 global host control                */
    uint32_t is;        /* 0x08 interrupt status                   */
    uint32_t pi;        /* 0x0C ports implemented                  */
    uint32_t vs;        /* 0x10 version                            */
    uint32_t ccc_ctl;   /* 0x14                                    */
    uint32_t ccc_pts;   /* 0x18                                    */
    uint32_t em_loc;    /* 0x1C                                    */
    uint32_t em_ctl;    /* 0x20                                    */
    uint32_t cap2;      /* 0x24                                    */
    uint32_t bohc;      /* 0x28 BIOS/OS handoff control            */
    uint8_t  rsv[0xA0 - 0x2C];
    uint8_t  vendor[0x100 - 0xA0];
    hba_port_t ports[32]; /* first port at 0x100                   */
} hba_mem_t;

_Static_assert(sizeof(hba_port_t) == 0x80, "HBA port register block must be 0x80 bytes");

/* ---- DMA structures (shared with hardware: layout must be exact) ---------- */
typedef struct tag_hba_cmd_header {
    uint8_t  cmd_fis_length:5; /* DW0: command FIS length in dwords (2-16) */
    uint8_t  atapi:1;
    uint8_t  write:1;          /* 1 = host -> device                        */
    uint8_t  prefetchable:1;
    uint8_t  reset:1;
    uint8_t  bist:1;
    uint8_t  clear_busy:1;
    uint8_t  rsv0:1;
    uint8_t  pmp:4;
    uint16_t prdt_length;      /* number of PRDT entries                    */
    uint32_t prdb_count;       /* DW1: bytes transferred (written by HW)    */
    uint32_t ctba;             /* DW2: command table base (128B aligned)    */
    uint32_t ctbau;            /* DW3                                       */
    uint32_t rsv1[4];
} __attribute__((packed)) hba_cmd_header_t;

typedef struct tag_hba_prdt_entry {
    uint32_t dba;              /* data base address                         */
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc:22;           /* byte count - 1, max 4 MiB                 */
    uint32_t rsv1:9;
    uint32_t i:1;              /* interrupt on completion                   */
} __attribute__((packed)) hba_prdt_entry_t;

typedef struct tag_hba_cmd_tbl {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  rsv[48];
    hba_prdt_entry_t prdt_entry[1]; /* variable length */
} __attribute__((packed)) hba_cmd_tbl_t;

_Static_assert(sizeof(hba_cmd_header_t) == 32, "command header must be 32 bytes");
_Static_assert(sizeof(hba_prdt_entry_t) == 16, "PRDT entry must be 16 bytes");
_Static_assert(offsetof(hba_cmd_tbl_t, prdt_entry) == 0x80, "PRDT must start at 0x80");

/* ---- Driver state --------------------------------------------------------- */
typedef struct ahci_device {
    int port_no;
    int implemented;
    int type;
    uint32_t lock;
    hba_port_t *hba_port;

    /* DMA buffer descriptors */
    dma_buf_t cl_buf;
    dma_buf_t fb_buf;
    dma_buf_t ct_buf;
    size_t    ct_size;

    void     *cl_virt;
    uint64_t  cl_phys;
    void     *fb_virt;
    uint64_t  fb_phys;
    void     *ct_virt;
    uint64_t  ct_phys;
} ahci_device_t;

/* Returns the number of usable ports brought up (>= 0). */
int  ahci_init(pci_device_t *pci_dev);

/* Returns port number of the first usable SATA disk, or -1. */
int  ahci_first_sata_port(void);

/* Buffers MUST be addressable through the HHDM (heap / dma_alloc_coherent).
 * Kernel image (.data/.bss) and non-HHDM stacks are NOT valid DMA targets.
 * Return 0 on success, negative E_* on failure. */
int  ahci_read (int port_no, uint64_t lba, uint32_t sector_count, void *buf);
int  ahci_write(int port_no, uint64_t lba, uint32_t sector_count, const void *buf);

#endif /* DRIVERS_AHCI_H */