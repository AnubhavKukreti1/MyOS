/*
- MyOS - src/drivers/fb_console.c
- Part of the MyOS 0.2.0 kernel (split out of the monolithic main.c).
 */

/* #########################################################################
   FRAMEBUFFER TEXT CONSOLE
   -------------------------------------------------------------------------
   Minimal scrolling text renderer using the public-domain 8x8 "font8x8"
   glyph set (ASCII 0x20-0x7E, one byte per row, LSB = leftmost pixel).
   Limited to 32 bpp RGB, which is what Limine provides in practice on UEFI
   (GOP) and modern BIOS VBE; anything else disables the console rather
   than risking garbage writes.

   Scrolling reads the framebuffer back, which is slow on write-combined
   memory. Once a heap exists, keep a shadow buffer and blit instead.
   ######################################################################### */

#include "kernel.h"
#include "font8x8.h"

#define FONT_W 8u
#define FONT_H 8u

static struct {
    bool  ready;
    u8   *base;
    u32   width, height, pitch;
    u8    r_size, r_shift, g_size, g_shift, b_size, b_shift;
    u32   scale;                 /* integer glyph magnification          */
    u32   cell_w, cell_h;
    u32   cols, rows, cx, cy;
    u32   fg_px, bg_px;          /* colours already packed for this mode */
} fbc;

static u32 fb_pack(u32 rgb) {
    u32 r = (rgb >> 16) & 0xFF;
    u32 g = (rgb >> 8) & 0xFF;
    u32 b = rgb & 0xFF;

    return ((r >> (8 - fbc.r_size)) << fbc.r_shift) |
           ((g >> (8 - fbc.g_size)) << fbc.g_shift) |
           ((b >> (8 - fbc.b_size)) << fbc.b_shift);
}

static void fb_fill_rows(u32 y0, u32 y1, u32 px) {
    for (u32 y = y0; y < y1; y++) {
        volatile u32 *line = (volatile u32 *)(fbc.base + (u64)y * fbc.pitch);
        for (u32 x = 0; x < fbc.width; x++) {
            line[x] = px;
        }
    }
}

bool fb_console_init(const fb_info_t *info) {
    if (info == NULL || info->address == NULL) {
        return false;
    }
    /* Only the common 32-bpp case; refuse everything else explicitly. */
    if (info->bpp != 32 || info->width < FONT_W || info->height < FONT_H ||
        info->pitch < info->width * 4 ||
        info->red_size > 8 || info->green_size > 8 || info->blue_size > 8) {
        return false;
    }

    fbc.base    = info->address;
    fbc.width   = (u32)info->width;
    fbc.height  = (u32)info->height;
    fbc.pitch   = (u32)info->pitch;
    fbc.r_size  = info->red_size;   fbc.r_shift = info->red_shift;
    fbc.g_size  = info->green_size; fbc.g_shift = info->green_shift;
    fbc.b_size  = info->blue_size;  fbc.b_shift = info->blue_shift;

    fbc.scale  = (fbc.width >= 1600) ? 2 : 1;   /* keep text legible on 4K-ish */
    fbc.cell_w = FONT_W * fbc.scale;
    fbc.cell_h = FONT_H * fbc.scale;
    fbc.cols   = fbc.width / fbc.cell_w;
    fbc.rows   = fbc.height / fbc.cell_h;
    fbc.cx = fbc.cy = 0;

    fbc.fg_px = fb_pack(COLOR_WHITE);
    fbc.bg_px = fb_pack(COLOR_BG);
    fbc.ready = true;

    fb_console_clear();
    return true;
}

bool fb_console_ready(void) {
    return fbc.ready;
}

void fb_console_set_colors(u32 fg_rgb, u32 bg_rgb) {
    if (!fbc.ready) {
        return;
    }
    fbc.fg_px = fb_pack(fg_rgb);
    fbc.bg_px = fb_pack(bg_rgb);
}

void fb_console_set_fg(u32 fg_rgb) {
    if (fbc.ready) {
        fbc.fg_px = fb_pack(fg_rgb);
    }
}

void fb_console_clear(void) {
    if (!fbc.ready) {
        return;
    }
    fb_fill_rows(0, fbc.height, fbc.bg_px);
    fbc.cx = fbc.cy = 0;
}

static void fb_draw_glyph(u32 col, u32 row, char ch) {
    unsigned idx = (unsigned char)ch;
    if (idx < 0x20 || idx > 0x7E) {
        idx = '?';
    }
    const u8 *glyph = font8x8[idx - 0x20];
    u32 px0 = col * fbc.cell_w;
    u32 py0 = row * fbc.cell_h;

    for (u32 gy = 0; gy < FONT_H; gy++) {
        u8 bits = glyph[gy];
        for (u32 sy = 0; sy < fbc.scale; sy++) {
            volatile u32 *line = (volatile u32 *)
                (fbc.base + (u64)(py0 + gy * fbc.scale + sy) * fbc.pitch);
            for (u32 gx = 0; gx < FONT_W; gx++) {
                u32 px = ((bits >> gx) & 1u) ? fbc.fg_px : fbc.bg_px;
                for (u32 sx = 0; sx < fbc.scale; sx++) {
                    line[px0 + gx * fbc.scale + sx] = px;
                }
            }
        }
    }
}

static void fb_scroll(void) {
    u64 row_bytes = (u64)fbc.cell_h * fbc.pitch;

    memmove(fbc.base, fbc.base + row_bytes, row_bytes * (fbc.rows - 1));
    fb_fill_rows((fbc.rows - 1) * fbc.cell_h, fbc.rows * fbc.cell_h, fbc.bg_px);
}

static void fb_newline(void) {
    fbc.cx = 0;
    if (++fbc.cy >= fbc.rows) {
        fbc.cy = fbc.rows - 1;
        fb_scroll();
    }
}

void fb_console_putc(char c) {
    if (!fbc.ready) {
        return;
    }
    switch (c) {
    case '\n':
        fb_newline();
        return;
    case '\r':
        fbc.cx = 0;
        return;
    case '\t':
        fbc.cx = (fbc.cx + 4) & ~3u;
        if (fbc.cx >= fbc.cols) {
            fb_newline();
        }
        return;
    case '\b':
        if (fbc.cx > 0) {
            fbc.cx--;
            fb_draw_glyph(fbc.cx, fbc.cy, ' ');
        }
        return;
    default:
        break;
    }
    if ((unsigned char)c < 0x20) {
        return;   /* swallow other control characters */
    }
    if (fbc.cx >= fbc.cols) {
        fb_newline();
    }
    fb_draw_glyph(fbc.cx, fbc.cy, c);
    fbc.cx++;
}