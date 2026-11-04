/*
 * MyOS - My First Operating System
 * Version: 0.1.0
 * Architecture: x86_64
 *
 * Milestone 1: Proper Kernel Initialization
 */

#include <stdint.h>
#include <stddef.h>
#include <limine.h>

/* =========================================
   TYPE DEFINITIONS
   ========================================= */

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint64_t u64;

/* =========================================
   LIMINE BASE REVISION
   ========================================= */

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[3] =
    LIMINE_BASE_REVISION(3);

/* =========================================
   LIMINE REQUEST MARKERS
   ========================================= */

__attribute__((used, section(".limine_requests_start"), aligned(8)))
static volatile uint64_t limine_requests_start[4] =
    LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end"), aligned(8)))
static volatile uint64_t limine_requests_end[2] =
    LIMINE_REQUESTS_END_MARKER;

/* =========================================
   HHDM REQUEST
   ========================================= */

__attribute__((used, section(".limine_requests"), aligned(8)))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

/* =========================================
   MEMORY MAP REQUEST
   ========================================= */

__attribute__((used, section(".limine_requests"), aligned(8)))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

/* =========================================
   KERNEL ADDRESS REQUEST
   ========================================= */

__attribute__((used, section(".limine_requests"), aligned(8)))
static volatile struct limine_kernel_address_request kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST_ID,
    .revision = 0,
    .response = NULL
};

/* =========================================
   SERIAL DRIVER - COM1
   ========================================= */

#define COM1 0x3F8

static inline void outb(u16 port, u8 value) {
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline u8 inb(u16 port) {
    u8 value;

    __asm__ volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

/* Initialize serial port */

static void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

/* Print one character */

static void serial_write_char(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) {
        __asm__ volatile ("pause");
    }

    outb(COM1, (u8)c);
}

/* Print string */

static void serial_write(const char *str) {
    while (*str) {
        if (*str == '\n') {
            serial_write_char('\r');
        }

        serial_write_char(*str++);
    }
}

/* =========================================
   NUMBER PRINTING
   ========================================= */

static void serial_write_u64(u64 value) {
    char buffer[21];
    int i = 0;

    if (value == 0) {
        serial_write_char('0');
        return;
    }

    while (value > 0) {
        buffer[i++] = '0' + (value % 10);
        value /= 10;
    }

    while (i > 0) {
        serial_write_char(buffer[--i]);
    }
}

static void serial_write_hex(u64 value) {
    const char hex[] = "0123456789ABCDEF";

    serial_write("0x");

    int started = 0;

    for (int i = 60; i >= 0; i -= 4) {
        u8 digit = (value >> i) & 0xF;

        if (digit != 0 || started || i == 0) {
            serial_write_char(hex[digit]);
            started = 1;
        }
    }
}

/* =========================================
   MEMORY MAP TYPE NAMES
   ========================================= */

static const char *memmap_type_name(u64 type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE:
            return "USABLE";

        case LIMINE_MEMMAP_RESERVED:
            return "RESERVED";

        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
            return "ACPI_RECLAIMABLE";

        case LIMINE_MEMMAP_ACPI_NVS:
            return "ACPI_NVS";

        case LIMINE_MEMMAP_BAD_MEMORY:
            return "BAD_MEMORY";

        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
            return "BOOTLOADER_RECLAIMABLE";

        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
            return "KERNEL_MODULES";

        case LIMINE_MEMMAP_FRAMEBUFFER:
            return "FRAMEBUFFER";

        default:
            return "UNKNOWN";
    }
}

/* =========================================
   PRINT MEMORY MAP
   ========================================= */

static void print_memory_map(void) {
    struct limine_memmap_response *response =
        memmap_request.response;

    if (response == NULL) {
        serial_write("ERROR: Memory map unavailable.\n");
        return;
    }

    serial_write("\n========== MEMORY MAP ==========\n");

    u64 usable_memory = 0;

    serial_write("Total regions: ");
    serial_write_u64(response->entry_count);
    serial_write("\n\n");

    for (u64 i = 0; i < response->entry_count; i++) {
        struct limine_memmap_entry *entry =
            response->entries[i];

        if (entry == NULL) {
            serial_write("WARNING: Null memory map entry.\n");
            continue;
        }

        serial_write("Region ");
        serial_write_u64(i);
        serial_write("\n");

        serial_write("  Base: ");
        serial_write_hex(entry->base);
        serial_write("\n");

        serial_write("  Length: ");
        serial_write_u64(entry->length);
        serial_write(" bytes\n");

        serial_write("  Type: ");
        serial_write(memmap_type_name(entry->type));
        serial_write("\n\n");

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            usable_memory += entry->length;
        }
    }

    serial_write("========== SUMMARY ==========\n");

    serial_write("Total usable memory: ");
    serial_write_u64(usable_memory);
    serial_write(" bytes\n");

    serial_write("Usable memory (MiB): ");
    serial_write_u64(usable_memory / (1024 * 1024));
    serial_write(" MiB\n");
}

/* =========================================
   HHDM INFORMATION
   ========================================= */

static void print_hhdm_info(void) {
    if (hhdm_request.response == NULL) {
        serial_write("ERROR: HHDM unavailable.\n");
        return;
    }

    u64 offset = hhdm_request.response->offset;

    serial_write("HHDM: Available.\n");

    serial_write("HHDM offset: ");
    serial_write_hex(offset);
    serial_write("\n");
}

/* =========================================
   KERNEL ADDRESS INFORMATION
   ========================================= */

static void print_kernel_address(void) {
    if (kernel_address_request.response == NULL) {
        serial_write("ERROR: Kernel address unavailable.\n");
        return;
    }

    struct limine_kernel_address_response *response =
        kernel_address_request.response;

    serial_write("\n========== KERNEL ADDRESS ==========\n");

    serial_write("Physical base: ");
    serial_write_hex(response->physical_base);
    serial_write("\n");

    serial_write("Virtual base: ");
    serial_write_hex(response->virtual_base);
    serial_write("\n");
}

/* =========================================
   KERNEL INITIALIZATION
   ========================================= */

static void kernel_initialize(void) {
    serial_write("\n");
    serial_write("====================================\n");
    serial_write("         MyOS INITIALIZATION         \n");
    serial_write("====================================\n");

    /* Check Limine base revision */

    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        serial_write("ERROR: Unsupported Limine revision!\n");

        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    serial_write("[OK] Limine protocol supported.\n");

    /* HHDM */

    if (hhdm_request.response == NULL) {
        serial_write("[FAIL] HHDM response missing.\n");
    } else {
        serial_write("[OK] HHDM initialized.\n");
    }

    /* Memory map */

    if (memmap_request.response == NULL) {
        serial_write("[FAIL] Memory map response missing.\n");
    } else {
        serial_write("[OK] Memory map received.\n");
    }

    /* Kernel address */

    if (kernel_address_request.response == NULL) {
        serial_write("[FAIL] Kernel address response missing.\n");
    } else {
        serial_write("[OK] Kernel address received.\n");
    }

    /* Display information */

    print_hhdm_info();
    print_kernel_address();
    print_memory_map();

    serial_write("\n====================================\n");
    serial_write("       MyOS INITIALIZATION DONE      \n");
    serial_write("====================================\n");
}

/* =========================================
   KERNEL ENTRY POINT
   ========================================= */

__attribute__((noreturn))
void kernel_main(void) {
    serial_init();

    kernel_initialize();

    serial_write("\nMyOS 0.1.0 is running!\n");

    while (1) {
        __asm__ volatile ("hlt");
    }
}
