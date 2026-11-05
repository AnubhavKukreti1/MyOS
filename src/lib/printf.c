/*
 * MyOS - src/lib/printf.c
 * Part of the MyOS 0.2.0 kernel (split out of the monolithic main.c).
 */

/* #########################################################################
   FORMATTING ENGINE
   -------------------------------------------------------------------------
   One core (kvformat) that pushes characters into a callback. The console
   logger, snprintf and any future sink (ring buffer, network console) all
   reuse it, so format semantics are identical everywhere and there is no
   intermediate buffer to overflow.
   ######################################################################### */

#include "kernel.h"

struct fmt_state {
    fmt_out_fn out;
    void      *ctx;
    size_t     count;
};

typedef struct {
    bool   left;      /* '-' */
    bool   zero;      /* '0' */
    bool   is_long;   /* l, ll, z */
    size_t width;
} fmt_spec_t;

static void fs_putc(struct fmt_state *fs, char c) {
    fs->out(c, fs->ctx);
    fs->count++;
}

static void fs_pad(struct fmt_state *fs, size_t n, char c) {
    while (n--) {
        fs_putc(fs, c);
    }
}

static void fs_text(struct fmt_state *fs, const char *s, size_t len,
                    const fmt_spec_t *sp) {
    size_t pad = (sp->width > len) ? sp->width - len : 0;

    if (!sp->left) {
        fs_pad(fs, pad, ' ');
    }
    for (size_t i = 0; i < len; i++) {
        fs_putc(fs, s[i]);
    }
    if (sp->left) {
        fs_pad(fs, pad, ' ');
    }
}

/* prefix is "", "-" or "0x"; it counts toward the width and, with the '0'
 * flag, sits BEFORE the zero padding (e.g. 0x00ff, -0042). */
static void fs_number(struct fmt_state *fs, u64 value, unsigned base,
                      bool upper, const char *prefix, const fmt_spec_t *sp) {
    const char *digit_set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char digits[24];   /* u64 in base 10 needs at most 20 */
    size_t n = 0;

    do {
        digits[n++] = digit_set[value % base];
        value /= base;
    } while (value != 0);

    size_t plen  = kstrlen(prefix);
    size_t total = n + plen;
    size_t pad   = (sp->width > total) ? sp->width - total : 0;

    if (!sp->left && !sp->zero) {
        fs_pad(fs, pad, ' ');
    }
    for (size_t i = 0; i < plen; i++) {
        fs_putc(fs, prefix[i]);
    }
    if (!sp->left && sp->zero) {
        fs_pad(fs, pad, '0');
    }
    while (n > 0) {
        fs_putc(fs, digits[--n]);
    }
    if (sp->left) {
        fs_pad(fs, pad, ' ');
    }
}

size_t kvformat(fmt_out_fn out, void *ctx, const char *fmt, va_list ap) {
    struct fmt_state fs = { .out = out, .ctx = ctx, .count = 0 };

    for (; *fmt != '\0'; fmt++) {
        if (*fmt != '%') {
            fs_putc(&fs, *fmt);
            continue;
        }
        fmt++;

        fmt_spec_t sp = { .left = false, .zero = false,
                          .is_long = false, .width = 0 };

        for (;; fmt++) {
            if (*fmt == '-')      sp.left = true;
            else if (*fmt == '0') sp.zero = true;
            else                  break;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            if (sp.width < 255) {
                sp.width = sp.width * 10 + (size_t)(*fmt - '0');
            }
            fmt++;
        }
        while (*fmt == 'l' || *fmt == 'z') {
            sp.is_long = true;   /* l, ll, z are all 64-bit on LP64 */
            fmt++;
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            i64 v = sp.is_long ? (i64)va_arg(ap, long) : (i64)va_arg(ap, int);
            bool neg = v < 0;
            /* Unsigned negate is well-defined even for INT64_MIN. */
            u64 mag = neg ? (u64)0 - (u64)v : (u64)v;
            fs_number(&fs, mag, 10, false, neg ? "-" : "", &sp);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            u64 v = sp.is_long ? (u64)va_arg(ap, unsigned long)
                               : (u64)va_arg(ap, unsigned int);
            unsigned base = (*fmt == 'u') ? 10 : 16;
            fs_number(&fs, v, base, *fmt == 'X', "", &sp);
            break;
        }
        case 'p': {
            void *p = va_arg(ap, void *);
            sp.zero = true;
            if (sp.width == 0) {
                sp.width = 18;   /* "0x" + 16 hex digits */
            }
            fs_number(&fs, (u64)(uintptr_t)p, 16, false, "0x", &sp);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            fs_text(&fs, &c, 1, &sp);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            fs_text(&fs, s, kstrlen(s), &sp);
            break;
        }
        case '%':
            fs_putc(&fs, '%');
            break;
        case '\0':
            return fs.count;   /* dangling '%' at end of string */
        default:
            fs_putc(&fs, '%');
            fs_putc(&fs, *fmt);
            break;
        }
    }
    return fs.count;
}

struct snprintf_ctx {
    char  *buf;
    size_t size;
    size_t len;
};

static void snprintf_out(char c, void *ctx) {
    struct snprintf_ctx *s = ctx;
    if (s->len + 1 < s->size) {
        s->buf[s->len] = c;
    }
    s->len++;
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct snprintf_ctx s = { .buf = buf, .size = size, .len = 0 };

    kvformat(snprintf_out, &s, fmt, ap);
    if (size > 0) {
        buf[(s.len < size) ? s.len : size - 1] = '\0';
    }
    return (int)s.len;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}