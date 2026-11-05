/*
 * MyOS - src/drivers/keyboard.c
 * PS/2 Keyboard Driver (Scancode Set 1).
 */

#include "kernel.h"
#include "arch/x86_64/idt.h"
#include "drivers/keyboard.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64
#define KBD_RING   128                        /* must be a power of two */

static const char k_normal[] =
    "\0" "\033" "1234567890-=" "\b\t"
    "qwertyuiop[]" "\n" "\0"
    "asdfghjkl;'`" "\0" "\\"
    "zxcvbnm,./" "\0" "*" "\0" " ";

static const char k_shift[] =
    "\0" "\033" "!@#$%^&*()_+" "\b\t"
    "QWERTYUIOP{}" "\n" "\0"
    "ASDFGHJKL:\"~" "\0" "|"
    "ZXCVBNM<>?" "\0" "*" "\0" " ";

_Static_assert(sizeof k_normal == 0x3A + 1, "k_normal must cover scancodes 0x00-0x39");
_Static_assert(sizeof k_shift  == 0x3A + 1, "k_shift must cover scancodes 0x00-0x39");

static struct {
    bool ext;
    u8   skip;        /* bytes left to ignore after E1 (Pause) */
    u8   mods;
} kbd;

static key_event_t        g_ring[KBD_RING];
static volatile u32       g_head, g_tail;

static void ring_push(const key_event_t *e) {
    u32 h = g_head;
    if (((h + 1) & (KBD_RING - 1)) == g_tail) {
        return;                                /* full: drop the key */
    }
    g_ring[h] = *e;
    __asm__ volatile ("" : : : "memory");
    g_head = (h + 1) & (KBD_RING - 1);
}

bool keyboard_poll(key_event_t *ev) {
    u32 t = g_tail;
    if (t == g_head) {
        return false;
    }
    *ev = g_ring[t];
    __asm__ volatile ("" : : : "memory");
    g_tail = (t + 1) & (KBD_RING - 1);
    return true;
}

char keyboard_getchar(void) {
    for (;;) {
        key_event_t ev;
        if (keyboard_poll(&ev) && ev.pressed && ev.ascii != 0) {
            return ev.ascii;
        }
        __asm__ volatile ("sti; hlt" : : : "memory");   /* wake on the next IRQ */
    }
}

static void update_mods(u16 keycode, bool pressed) {
    u8 bit = 0;

    switch (keycode) {
    case KEY_LSHIFT: bit = KMOD_LSHIFT; break;
    case KEY_RSHIFT: bit = KMOD_RSHIFT; break;
    case KEY_LCTRL:  bit = KMOD_LCTRL;  break;
    case KEY_RCTRL:  bit = KMOD_RCTRL;  break;
    case KEY_LALT:   bit = KMOD_LALT;   break;
    case KEY_RALT:   bit = KMOD_RALT;   break;
    case KEY_CAPS:
        if (pressed) {
            kbd.mods ^= KMOD_CAPS;            /* toggles on press only */
        }
        return;
    default:
        return;
    }
    if (pressed) {
        kbd.mods |= bit;
    } else {
        kbd.mods &= (u8)~bit;
    }
}

static char translate(u8 code, bool ext) {
    if (ext) {
        return (code == 0x1C) ? '\n' : (code == 0x35) ? '/' : 0;    /* keypad Enter, '/' */
    }
    if (code >= sizeof k_normal - 1) {
        return 0;
    }
    bool shift = (kbd.mods & (KMOD_LSHIFT | KMOD_RSHIFT)) != 0;
    bool ctrl  = (kbd.mods & (KMOD_LCTRL | KMOD_RCTRL)) != 0;
    bool caps  = (kbd.mods & KMOD_CAPS) != 0;
    char base  = k_normal[code];
    char c     = shift ? k_shift[code] : base;

    if (base >= 'a' && base <= 'z') {
        if (caps) {
            c = shift ? base : k_shift[code];        /* caps inverts shift for letters */
        }
        if (ctrl) {
            c = (char)(base - 'a' + 1);              /* Ctrl+A = 1 ... */
        }
    }
    return c;
}

static void process(u8 sc) {
    if (sc == 0xE0) { kbd.ext = true; return; }
    if (sc == 0xE1) { kbd.skip = 5;   return; }
    if (kbd.skip != 0) { kbd.skip--;  return; }

    bool pressed = (sc & 0x80) == 0;
    u8   code    = sc & 0x7F;
    bool ext     = kbd.ext;
    kbd.ext = false;

    u16 keycode = (u16)(code | (ext ? 0x100 : 0));
    update_mods(keycode, pressed);

    key_event_t ev = {
        .keycode = keycode,
        .ascii   = pressed ? translate(code, ext) : 0,
        .mods    = kbd.mods,
        .pressed = pressed,
    };
    ring_push(&ev);
}

static void kbd_irq(interrupt_frame_t *f) {
    UNUSED(f);
    while ((inb(PS2_STATUS) & 0x21) == 0x01) {      /* output full, not mouse data */
        process(inb(PS2_DATA));
    }
}

static bool ps2_wait_in(void) {                     /* data ready to read */
    for (u32 i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & 0x01) { return true; }
    }
    return false;
}

static bool ps2_wait_out(void) {                    /* safe to write */
    for (u32 i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & 0x02)) { return true; }
    }
    return false;
}

bool keyboard_init(void) {
    while (inb(PS2_STATUS) & 0x01) {                /* drain stale bytes */
        (void)inb(PS2_DATA);
    }

    /* Config byte: bit0 = IRQ1 enable, bit6 = translate set 2 -> set 1. */
    if (ps2_wait_out()) {
        outb(PS2_CMD, 0x20);
        if (ps2_wait_in()) {
            u8 cfg = inb(PS2_DATA) | 0x41;
            if (ps2_wait_out()) {
                outb(PS2_CMD, 0x60);
                if (ps2_wait_out()) {
                    outb(PS2_DATA, cfg);
                }
            }
        }
    }

    if (!irq_register(1, kbd_irq)) {
        return false;
    }
    kok("PS/2 keyboard on IRQ1");
    return true;
}