/*
 * MyOS - src/drivers/keyboard.h
 * PS/2 Keyboard driver interface (Scancode Set 1).
 */

#ifndef MYOS_DRIVERS_KEYBOARD_H
#define MYOS_DRIVERS_KEYBOARD_H

#include "kernel.h"

/* Modifier bitmasks for key_event_t.mods */
#define KMOD_LSHIFT 0x01
#define KMOD_RSHIFT 0x02
#define KMOD_LCTRL  0x04
#define KMOD_RCTRL  0x08
#define KMOD_LALT   0x10
#define KMOD_RALT   0x20
#define KMOD_CAPS   0x40

/* Standard Key Codes (Set 1 make code, with 0x100 prefix for extended E0 keys) */
#define KEY_ESC       0x001
#define KEY_BACKSPACE 0x00E
#define KEY_ENTER     0x01C
#define KEY_LCTRL     0x01D
#define KEY_LSHIFT    0x02A
#define KEY_RSHIFT    0x036
#define KEY_LALT      0x038
#define KEY_CAPS      0x03A

/* Function Keys */
#define KEY_F1        0x03B
#define KEY_F2        0x03C
#define KEY_F3        0x03D
#define KEY_F4        0x03E
#define KEY_F5        0x03F
#define KEY_F6        0x040
#define KEY_F7        0x041
#define KEY_F8        0x042
#define KEY_F9        0x043
#define KEY_F10       0x044
#define KEY_F11       0x057
#define KEY_F12       0x058

/* Extended Keys (E0 prefix) */
#define KEY_RCTRL     0x11D
#define KEY_RALT      0x138
#define KEY_HOME      0x147
#define KEY_UP        0x148
#define KEY_PGUP      0x149
#define KEY_LEFT      0x14B
#define KEY_RIGHT     0x14D
#define KEY_END       0x14F
#define KEY_DOWN      0x150
#define KEY_PGDN      0x151
#define KEY_INSERT    0x152
#define KEY_DELETE    0x153

/**
 * @brief Represents a parsed keyboard event.
 */
typedef struct {
    u16  keycode;    /* Set-1 make code, or | 0x100 if extended (E0) */
    char ascii;      /* Translated ASCII character, or 0 if none */
    u8   mods;       /* Active modifier bitmask (KMOD_*) at event time */
    bool pressed;    /* true = key pressed, false = key released */
} key_event_t;

/**
 * @brief Initialize the PS/2 keyboard controller and register IRQ handler.
 * @return true if successfully initialized, false otherwise.
 */
bool keyboard_init(void);

/**
 * @brief Poll for the next available keyboard event in a non-blocking manner.
 * @param ev Pointer to a key_event_t structure to populate if an event is pending.
 * @return true if an event was retrieved, false if the buffer is empty.
 */
bool keyboard_poll(key_event_t *ev);

/**
 * @brief Read the next ASCII character, blocking with CPU halt until one arrives.
 * @return The translated ASCII character.
 */
char keyboard_getchar(void);

#endif /* MYOS_DRIVERS_KEYBOARD_H */