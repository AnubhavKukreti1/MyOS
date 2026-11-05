/*
- MyOS - src/kernel/log.c
- Part of the MyOS 0.2.0 kernel (split out of the monolithic main.c).
 */

/* #########################################################################
   LOGGING
   -------------------------------------------------------------------------
   Two sinks (serial, framebuffer) behind one kputc(). Serial gets ANSI
   colours (terminals and `qemu -serial stdio` render them); the framebuffer
   gets the same palette via its foreground colour. Single-CPU for now: when
   SMP arrives, wrap klog() in a spinlock so lines from different cores do
   not interleave, and make the panic path trylock-and-ignore.
   ######################################################################### */

#include "kernel.h"

typedef struct {
    const char *tag;    /* fixed 7 columns so message text always aligns */
    const char *ansi;
    u32         rgb;
} log_style_t;

static const log_style_t k_styles[LOG_LEVEL_COUNT] = {
    [LOG_DEBUG] = { "[DEBUG]", "\x1b[90m",       COLOR_GRAY   },
    [LOG_INFO]  = { "[ INFO]", "\x1b[36m",       COLOR_CYAN   },
    [LOG_OK]    = { "[  OK ]", "\x1b[32m",       COLOR_GREEN  },
    [LOG_WARN]  = { "[ WARN]", "\x1b[33m",       COLOR_YELLOW },
    [LOG_ERROR] = { "[ERROR]", "\x1b[31m",       COLOR_RED    },
    [LOG_FATAL] = { "[FATAL]", "\x1b[1;97;41m",  COLOR_WHITE  },
};

static bool        g_log_serial = true;
static bool        g_log_fb     = false;
static log_level_t g_log_min    = LOG_INFO;

void klog_set_min_level(log_level_t level) { g_log_min = level; }
void klog_enable_serial(bool on)           { g_log_serial = on; }
void klog_enable_fb(bool on)               { g_log_fb = on && fb_console_ready(); }

void kputc(char c) {
    if (g_log_serial) {
        serial_putc(c);
    }
    if (g_log_fb) {
        fb_console_putc(c);
    }
}

void kputs(const char *s) {
    while (*s != '\0') {
        kputc(*s++);
    }
}

static void kputc_cb(char c, void *ctx) {
    UNUSED(ctx);
    kputc(c);
}

void klog_style(log_level_t level) {
    if (g_log_serial) {
        const char *a = k_styles[level].ansi;
        while (*a != '\0') {
            serial_putc(*a++);
        }
    }
    if (g_log_fb) {
        fb_console_set_fg(k_styles[level].rgb);
    }
}

void klog_style_reset(void) {
    if (g_log_serial) {
        const char *a = "\x1b[0m";
        while (*a != '\0') {
            serial_putc(*a++);
        }
    }
    if (g_log_fb) {
        fb_console_set_fg(COLOR_WHITE);
    }
}

void kvprintf(const char *fmt, va_list ap) {
    kvformat(kputc_cb, NULL, fmt, ap);
}

void kprintf(const char *fmt, ...) {
    irq_flags_t fl = irq_save();
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    irq_restore(fl);
}

void klog(log_level_t level, const char *fmt, ...) {
    if (level < g_log_min || level >= LOG_LEVEL_COUNT) {
        return;
    }
    irq_flags_t fl = irq_save();
    klog_style(level);
    kputs(k_styles[level].tag);
    klog_style_reset();
    kputc(' ');

    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kputc('\n');
    irq_restore(fl);
}

/* "1.99 GiB" style size formatting for human-readable memory reports. */
void fmt_size(char *buf, size_t n, u64 bytes) {
    static const char *const units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    u64 unit = 1;
    size_t u = 0;

    while (u + 1 < ARRAY_SIZE(units) && bytes >= unit * 1024) {
        unit *= 1024;
        u++;
    }
    if (u == 0) {
        ksnprintf(buf, n, "%lu B", bytes);
    } else {
        u64 whole = bytes / unit;
        u64 frac  = (bytes % unit) * 100 / unit;
        ksnprintf(buf, n, "%lu.%02lu %s", whole, frac, units[u]);
    }
}