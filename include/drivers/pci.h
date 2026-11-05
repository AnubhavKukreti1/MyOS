/**
 * @file pci.h
 * @brief PCI / PCIe configuration access, enumeration and capability parsing.
 *
 * Access methods:
 *   - ECAM (memory-mapped, 4 KiB per function) if an ACPI MCFG table is found
 *     and agrees with mechanism #1.
 *   - Configuration Mechanism #1 (ports 0xCF8 / 0xCFC, first 256 bytes).
 */
#ifndef MYOS_DRIVERS_PCI_H
#define MYOS_DRIVERS_PCI_H

#include "kernel.h"

#define PCI_MAX_DEVICES   256
#define PCI_MAX_ECAM      8
#define PCI_ANY_ID        0xFFFFu
#define PCI_ANY_PROGIF    (-1)

/* Standard configuration header offsets */
#define PCI_CFG_VENDOR_ID    0x00
#define PCI_CFG_DEVICE_ID    0x02
#define PCI_CFG_COMMAND      0x04
#define PCI_CFG_STATUS       0x06
#define PCI_CFG_REVISION     0x08
#define PCI_CFG_PROG_IF      0x09
#define PCI_CFG_SUBCLASS     0x0A
#define PCI_CFG_CLASS        0x0B
#define PCI_CFG_HEADER_TYPE  0x0E
#define PCI_CFG_BAR0         0x10
#define PCI_CFG_SUBSYS_VID   0x2C
#define PCI_CFG_SUBSYS_ID    0x2E
#define PCI_CFG_CAP_PTR      0x34
#define PCI_CFG_IRQ_LINE     0x3C
#define PCI_CFG_IRQ_PIN      0x3D
#define PCI_CFG_SECONDARY    0x19     /* PCI-PCI bridge */

#define PCI_CMD_IO           (1u << 0)
#define PCI_CMD_MEM          (1u << 1)
#define PCI_CMD_MASTER       (1u << 2)
#define PCI_CMD_INTX_OFF     (1u << 10)
#define PCI_STATUS_CAP_LIST  (1u << 4)

#define PCI_CAP_PM           0x01
#define PCI_CAP_MSI          0x05
#define PCI_CAP_PCIE         0x10
#define PCI_CAP_MSIX         0x11

#define PCI_EXT_CAP_AER      0x0001
#define PCI_EXT_CAP_SRIOV    0x0010

typedef struct {
    u16 seg;
    u8  bus, dev, fn;
} pci_addr_t;

typedef struct { u16 offset; u8 version; bool d1, d2; u8 pme_support; } pci_pm_cap_t;
typedef struct { u16 offset; bool is64; bool per_vector_mask; u8 max_vectors; } pci_msi_cap_t;
typedef struct { u16 offset; u16 table_size; u8 table_bir; u32 table_offset;
                 u8 pba_bir; u32 pba_offset; } pci_msix_cap_t;
typedef struct { u16 offset; u8 version; u8 port_type; u8 max_link_speed; u8 max_link_width; } pci_pcie_cap_t;

typedef struct pci_device {
    pci_addr_t addr;
    u16  vendor_id, device_id;
    u8   revision, prog_if, subclass, class_code;
    u8   header_type;                 /* without the multifunction bit */
    bool multifunction;
    bool is_bridge;
    u8   secondary_bus;
    u16  subsys_vendor, subsys_id;
    u8   irq_line, irq_pin;
    pci_pm_cap_t   pm;                /* .offset == 0 -> capability absent */
    pci_msi_cap_t  msi;
    pci_msix_cap_t msix;
    pci_pcie_cap_t pcie;
    volatile u32   claimed;           /* driver binding, see pci_claim() */
    void *driver_data;
} pci_device_t;

typedef enum { PCI_BAR_NONE = 0, PCI_BAR_IO, PCI_BAR_MEM32, PCI_BAR_MEM64 } pci_bar_type_t;

typedef struct {
    pci_bar_type_t type;
    bool prefetchable;
    u64  base;
    u64  size;
} pci_bar_t;

/**
 * @brief Detect config mechanisms, discover ECAM from ACPI, enumerate all buses.
 * @param rsdp_phys physical address of the ACPI RSDP, or 0 to scan the BIOS area
 * @return 0 or -E_NODEV
 */
int32_t pci_init(u64 rsdp_phys);

/** @brief Register an ECAM window (also used by pci_init for MCFG entries). */
int32_t pci_ecam_add(u16 seg, u8 start_bus, u8 end_bus, u64 phys);

u32 pci_device_count(void);
pci_device_t *pci_get_device(u32 index);
pci_device_t *pci_find_device(u16 vendor, u16 device, u32 index);   /* PCI_ANY_ID wildcards */
pci_device_t *pci_find_class(u8 class_code, u8 subclass, int16_t prog_if, u32 index);
const char *pci_class_name(u8 class_code, u8 subclass);

/* Raw configuration access: width is 1, 2 or 4; offset must be width-aligned. */
int32_t pci_addr_read(const pci_addr_t *a, u16 off, u8 width, u32 *val);
int32_t pci_addr_write(const pci_addr_t *a, u16 off, u8 width, u32 val);

int32_t pci_read_config_u8(const pci_device_t *d, u16 off, u8 *v);
int32_t pci_read_config_u16(const pci_device_t *d, u16 off, u16 *v);
int32_t pci_read_config_u32(const pci_device_t *d, u16 off, u32 *v);
int32_t pci_write_config_u8(const pci_device_t *d, u16 off, u8 v);
int32_t pci_write_config_u16(const pci_device_t *d, u16 off, u16 v);
int32_t pci_write_config_u32(const pci_device_t *d, u16 off, u32 v);

/** @brief Offset of a capability in the legacy list, or 0. */
u16 pci_find_capability(const pci_device_t *d, u8 cap_id);
/** @brief Offset of a PCIe extended capability (requires ECAM), or 0. */
u16 pci_find_ext_capability(const pci_device_t *d, u16 cap_id);

/** @brief Exclusive driver binding. Returns -E_BUSY if already claimed. */
int32_t pci_claim(pci_device_t *d);

/** @brief Set command-register bits (PCI_CMD_MEM | PCI_CMD_MASTER ...). */
int32_t pci_enable_device(pci_device_t *d, u16 cmd_bits);
int32_t pci_set_bus_master(pci_device_t *d, bool on);
int32_t pci_set_intx(pci_device_t *d, bool enable);

/** @brief Size and decode a BAR (decoding is disabled while probing). */
int32_t pci_get_bar(pci_device_t *d, u8 index, pci_bar_t *out);

/** @brief Move the function to D0 using the PM capability, if present. */
int32_t pci_pm_set_d0(pci_device_t *d);

/** @brief Program a single MSI vector and enable it (INTx is masked). */
int32_t pci_msi_enable(pci_device_t *d, u64 msg_addr, u16 msg_data);
int32_t pci_msi_disable(pci_device_t *d);

#endif