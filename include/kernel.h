/*
- MyOS - kernel.h
- Version: 0.2.0
 *
- Single internal header declaring every subsystem's public surface.
 *
- WHY one header (for now)?
-   Each section of main.c (serial, fmt, log, fb, cpu, gdt/idt, panic, boot,
-   mem) only talks to the others through the declarations below. That makes
-   the later split into serial.c / printf.c / ... a mechanical cut-and-paste:
-   move the section, keep the declarations. Nothing in here includes
-   <limine.h>: the bootloader protocol is quarantined inside the "boot"
-   section of main.c and the rest of the kernel only sees neutral structs
-   (boot_info_t, mem_region_t, fb_info_t). If you ever swap bootloaders,
-   only that one section changes.
 */

#ifndef MYOS_KERNEL_H
#define MYOS_KERNEL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* =========================================================================
   BASIC TYPES & MACROS
   ========================================================================= */

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  i64;

#define KERNEL_NAME     "MyOS"
#define KERNEL_VERSION  "0.2.0"

#define KiB  (1024UL)
#define MiB  (1024UL * KiB)
#define GiB  (1024UL * MiB)
#define TiB  (1024UL * GiB)

#define PAGE_SIZE        4096UL
#define ALIGN_UP(x, a)   (((x) + ((a) - 1ULL)) & ~((a) - 1ULL))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1ULL))
#define ARRAY_SIZE(a)    (sizeof(a) / sizeof((a)[0]))
#define UNUSED(x)        ((void)(x))

#define NORETURN         __attribute__((noreturn))
#define PACKED           __attribute__((packed))
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))

/* x86_64 canonical higher-half boundaries used for sanity checks. */
#define HIGHER_HALF_START   0xFFFF800000000000ULL
#define KERNEL_IMAGE_START  0xFFFFFFFF80000000ULL

/* Control-register / MSR bits shared by cpu.c and panic.c. */
#define MSR_EFER        0xC0000080u
#define EFER_LME        (1ULL << 8)
#define EFER_LMA        (1ULL << 10)
#define EFER_NXE        (1ULL << 11)
#define CR0_PE          (1ULL << 0)
#define CR0_WP          (1ULL << 16)
#define CR0_PG          (1ULL << 31)
#define CR4_PAE         (1ULL << 5)

/* =========================================================================
   LOW-LEVEL CPU PRIMITIVES (header-only, always inlined)
   ========================================================================= */

static inline void outb(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(u16 port, u16 value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline u16 inw(u16 port) {
    u16 value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(u16 port, u32 value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline u32 inl(u16 port) {
    u32 value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void cpu_relax(void) {
    __asm__ volatile ("pause" : : : "memory");
}

static inline void cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(sub));
}

static inline u64 rdmsr(u32 msr) {
    u32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static inline u64 read_cr0(void) { u64 v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v; }
static inline u64 read_cr2(void) { u64 v; __asm__ volatile ("mov %%cr2, %0" : "=r"(v)); return v; }
static inline u64 read_cr3(void) { u64 v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v; }
static inline u64 read_cr4(void) { u64 v; __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); return v; }

static inline u64 read_rflags(void) {
    u64 v;
    __asm__ volatile ("pushfq; popq %0" : "=r"(v) : : "memory");
    return v;
}

static inline void wrmsr(u32 msr, u64 v) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((u32)v), "d"((u32)(v >> 32)) : "memory");
}
static inline void write_cr0(u64 v) { __asm__ volatile ("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline void write_cr3(u64 v) { __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline void invlpg(const void *a) { __asm__ volatile ("invlpg (%0)" : : "r"(a) : "memory"); }

typedef u64 irq_flags_t;
static inline irq_flags_t irq_save(void) {
    u64 f;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(f) : : "memory");
    return f;
}
static inline void irq_restore(irq_flags_t f) {
    if (f & (1ULL << 9)) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

#define MSR_PAT      0x277u
#define IDT_VECTORS  256
#define GDT_TSS      0x28

static inline NORETURN void halt_forever(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

/* =========================================================================
   FREESTANDING LIBC SUBSET
   GCC may emit calls to these even if we never write them (struct copies,
   loop idiom recognition), so they are mandatory in a freestanding kernel.
   ========================================================================= */

void  *memcpy(void *restrict dest, const void *restrict src, size_t n);
void  *memmove(void *dest, const void *src, size_t n);
void  *memset(void *dest, int value, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t kstrlen(const char *s);

/* =========================================================================
   SERIAL (COM1)
   ========================================================================= */

bool serial_init(void);
bool serial_is_ready(void);
void serial_putc(char c);   /* translates '\n' -> "\r\n" */

/* =========================================================================
   FORMATTING ENGINE
   Supported: %s %c %d %i %u %x %X %p %%   flags: '-' '0'   width: digits
   length: l, ll, z (all treated as 64-bit on LP64)
   ========================================================================= */

typedef void (*fmt_out_fn)(char c, void *ctx);

size_t kvformat(fmt_out_fn out, void *ctx, const char *fmt, va_list ap);
int    kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
           PRINTF_LIKE(3, 0);
int    ksnprintf(char *buf, size_t size, const char *fmt, ...)
           PRINTF_LIKE(3, 4);

/* =========================================================================
   FRAMEBUFFER TEXT CONSOLE
   ========================================================================= */

typedef struct {
    void *address;
    u64   width;
    u64   height;
    u64   pitch;
    u16   bpp;
    u8    red_size,   red_shift;
    u8    green_size, green_shift;
    u8    blue_size,  blue_shift;
} fb_info_t;

#define COLOR_BLACK     0x000000u
#define COLOR_WHITE     0xE6E6E6u
#define COLOR_GRAY      0x8A9099u
#define COLOR_RED       0xFF5555u
#define COLOR_GREEN     0x50FA7Bu
#define COLOR_YELLOW    0xF1FA8Cu
#define COLOR_CYAN      0x8BE9FDu
#define COLOR_BG        0x101418u
#define COLOR_PANIC_BG  0x5A0000u

bool fb_console_init(const fb_info_t *info);
bool fb_console_ready(void);
void fb_console_set_colors(u32 fg_rgb, u32 bg_rgb);
void fb_console_set_fg(u32 fg_rgb);
void fb_console_clear(void);
void fb_console_putc(char c);

/* =========================================================================
   LOGGING
   ========================================================================= */

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_OK,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL,
    LOG_LEVEL_COUNT
} log_level_t;

void klog(log_level_t level, const char *fmt, ...) PRINTF_LIKE(2, 3);
void kprintf(const char *fmt, ...) PRINTF_LIKE(1, 2);
void kvprintf(const char *fmt, va_list ap) PRINTF_LIKE(1, 0);
void kputs(const char *s);
void kputc(char c);

void klog_set_min_level(log_level_t level);
void klog_enable_serial(bool on);
void klog_enable_fb(bool on);
void klog_style(log_level_t level);   /* switch sinks to a level's colour */
void klog_style_reset(void);
void fmt_size(char *buf, size_t n, u64 bytes);   /* "1.99 GiB" */

#define kdebug(...) klog(LOG_DEBUG, __VA_ARGS__)
#define kinfo(...)  klog(LOG_INFO,  __VA_ARGS__)
#define kok(...)    klog(LOG_OK,    __VA_ARGS__)
#define kwarn(...)  klog(LOG_WARN,  __VA_ARGS__)
#define kerror(...) klog(LOG_ERROR, __VA_ARGS__)

/* =========================================================================
   CPU, GDT, IDT, EXCEPTIONS
   ========================================================================= */

typedef struct {
    char vendor[13];
    char brand[49];
    u32  max_basic;
    u32  max_ext;
    u8   phys_bits;
    u8   virt_bits;
    bool long_mode;
    bool nx;
    bool page1g;
    bool sse2;
    bool apic;
    bool x2apic;
    bool pat;
    bool invariant_tsc;
    u64  cr0, cr3, cr4, efer;
} cpu_info_t;

void cpu_early_init(cpu_info_t *ci);
void cpu_report(const cpu_info_t *ci);

#define GDT_KERNEL_CS 0x08
#define GDT_KERNEL_DS 0x10
#define GDT_USER_DS   0x18
#define GDT_USER_CS   0x20

/* Operand of lgdt/lidt. */
typedef struct PACKED {
    u16 limit;
    u64 base;
} dtr_t;

#define EXC_VECTORS 32

void gdt_init(void);
void tss_set_rsp0(u64 rsp0);
void idt_init(void);

/* Layout MUST match the push order in isr_common (see main.c). */
typedef struct {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector;
    u64 error_code;
    u64 rip, cs, rflags, rsp, ss;   /* pushed by the CPU */
} interrupt_frame_t;

typedef void (*exc_handler_t)(interrupt_frame_t *frame);

bool idt_register_handler(u8 vector, exc_handler_t handler);
interrupt_frame_t *exception_dispatch(interrupt_frame_t *frame);   /* called from asm */

/* =========================================================================
   PANIC / ASSERT
   ========================================================================= */

NORETURN void kpanic_with_frame(const char *file, int line,
                                const interrupt_frame_t *frame,
                                const char *fmt, ...) PRINTF_LIKE(4, 5);

#define kpanic(...) \
    kpanic_with_frame(__FILE__, __LINE__, NULL, __VA_ARGS__)
#define kpanic_frame(frame, ...) \
    kpanic_with_frame(__FILE__, __LINE__, (frame), __VA_ARGS__)
#define KASSERT(cond) \
    do { if (__builtin_expect(!(cond), 0)) kpanic("Assertion failed: %s", #cond); } while (0)

/* =========================================================================
   MEMORY (early / PMM preparation)
   ========================================================================= */

typedef enum {
    MEMT_USABLE = 0,
    MEMT_RESERVED,
    MEMT_ACPI_RECLAIMABLE,
    MEMT_ACPI_NVS,
    MEMT_BAD,
    MEMT_BOOTLOADER_RECLAIMABLE,
    MEMT_KERNEL_AND_MODULES,
    MEMT_FRAMEBUFFER,
    MEMT_UNKNOWN,
    MEMT_COUNT
} mem_type_t;

typedef struct {
    u64        base;
    u64        length;
    mem_type_t type;
} mem_region_t;

#define MEM_MAX_REGIONS 128

typedef struct {
    const mem_region_t *regions;
    size_t              count;
    bool                truncated;     /* bootloader gave more than we kept */
    u64                 bytes_by_type[MEMT_COUNT];
    u64                 usable_conventional;  /* usable below 1 MiB      */
    u64                 usable_low;           /* usable 1 MiB .. 4 GiB   */
    u64                 usable_high;          /* usable at/above 4 GiB   */
    u64                 usable_frames;        /* whole 4 KiB frames      */
    u64                 highest_usable_end;   /* page-aligned, exclusive */
    unsigned            anomalies;            /* overlap/order violations */
} mem_info_t;

typedef struct {
    bool   found;
    u64    span_frames;     /* frames covered by the bitmap (0..highest)   */
    u64    bitmap_bytes;    /* page-aligned                                */
    u64    bitmap_phys;     /* where the PMM can carve its bitmap          */
    size_t region_index;
} pmm_plan_t;

/* =========================================================================
   BOOT INFORMATION (bootloader-neutral)
   ========================================================================= */

typedef enum {
    BOOT_CHK_BASE_REV = 0,
    BOOT_CHK_HHDM,
    BOOT_CHK_MEMMAP,
    BOOT_CHK_KADDR,
    BOOT_CHK_FRAMEBUFFER,
    BOOT_CHK_COUNT
} boot_check_id_t;

typedef struct {
    const char *name;
    const char *detail;
    bool        ok;
    bool        fatal;   /* kernel cannot continue without it */
} boot_check_t;

typedef struct {
    boot_check_t checks[BOOT_CHK_COUNT];
    u64          hhdm_offset;
    u64          kernel_phys_base;
    u64          kernel_virt_base;
    mem_region_t regions[MEM_MAX_REGIONS];
    size_t       region_count;
    size_t       region_total;   /* what the bootloader actually reported */
    fb_info_t    fb;
} boot_info_t;

extern boot_info_t g_boot;

void boot_collect(boot_info_t *bi);
void boot_report_and_enforce(const boot_info_t *bi);
void boot_reclaim_memory(void);


/* Higher-Half Direct Map helpers: the only sanctioned way to touch
- physical memory until we build our own page tables. */
static inline void *phys_to_virt(u64 phys) {
    return (void *)(uintptr_t)(phys + g_boot.hhdm_offset);
}
static inline u64 virt_to_phys_hhdm(const void *virt) {
    return (u64)(uintptr_t)virt - g_boot.hhdm_offset;
}

void mem_init(const boot_info_t *bi, mem_info_t *mi);
void mem_report(const mem_info_t *mi);
bool pmm_plan_bitmap(const mem_info_t *mi, pmm_plan_t *plan);
void pmm_report(const mem_info_t *mi, const pmm_plan_t *plan);
const char *mem_type_name(mem_type_t t);

/* =========================================================================
   ENTRY POINT
   ========================================================================= */

NORETURN void kernel_main(void);

#endif /* MYOS_KERNEL_H */