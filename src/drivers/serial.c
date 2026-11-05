/*
 * MyOS - src/drivers/serial.c
 * Part of the MyOS 0.2.0 kernel (split out of the monolithic main.c).
 */

/* #########################################################################
   SERIAL DRIVER (COM1)
   -------------------------------------------------------------------------
   Polled 16550 UART. We run a loopback self-test so that on hardware with no
   UART (or an emulated one that is disabled) we silently drop output instead
   of spinning forever. Transmit waits are bounded for the same reason.
   ######################################################################### */

#include "kernel.h"

#define COM1            0x3F8
#define COM_DATA        (COM1 + 0)
#define COM_IER         (COM1 + 1)
#define COM_FCR         (COM1 + 2)
#define COM_LCR         (COM1 + 3)
#define COM_MCR         (COM1 + 4)
#define COM_LSR         (COM1 + 5)
#define COM_LSR_THRE    0x20
#define SERIAL_SPIN_MAX 200000u

static bool g_serial_ok = false;

bool serial_init(void) {
    outb(COM_IER, 0x00);   /* no UART interrupts: we poll                 */
    outb(COM_LCR, 0x80);   /* DLAB on: expose baud divisor                */
    outb(COM_DATA, 0x03);  /* divisor 3 -> 38400 baud (lo)                */
    outb(COM_IER, 0x00);   /*                          (hi)               */
    outb(COM_LCR, 0x03);   /* 8 data bits, no parity, 1 stop (8N1)        */
    outb(COM_FCR, 0xC7);   /* enable + clear FIFOs, 14-byte threshold     */

    /* Loopback self-test: MCR bit 4 routes TX back to RX internally. */
    outb(COM_MCR, 0x1E);
    outb(COM_DATA, 0xAE);
    if (inb(COM_DATA) != 0xAE) {
        g_serial_ok = false;
        return false;
    }

    outb(COM_MCR, 0x0F);   /* normal operation: DTR, RTS, OUT1, OUT2      */
    g_serial_ok = true;
    return true;
}

bool serial_is_ready(void) {
    return g_serial_ok;
}

static void serial_raw_putc(char c) {
    for (u32 spin = 0; spin < SERIAL_SPIN_MAX; spin++) {
        if (inb(COM_LSR) & COM_LSR_THRE) {
            outb(COM_DATA, (u8)c);
            return;
        }
        __asm__ volatile ("pause");
    }
    /* Transmitter stuck: drop the byte rather than hang the kernel. */
}

void serial_putc(char c) {
    if (!g_serial_ok) {
        return;
    }
    if (c == '\n') {
        serial_raw_putc('\r');
    }
    serial_raw_putc(c);
}