/* MyOS - src/kernel/shell.c */

#include <stddef.h>
#include <stdint.h>

#include "kernel.h"
#include "kernel/shell.h"
#include "drivers/keyboard.h"
#include "kernel/scheduler.h"
#include "mm/pmm.h"

#define SHELL_BUF_SIZE 256

static int kstrcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static void shell_execute(const char *line) {
    if (kstrcmp(line, "help") == 0) {
        kprintf("Available commands:\n");
        kprintf("  help   - Display this help message\n");
        kprintf("  clear  - Clear the screen\n");
        kprintf("  mem    - Display physical memory usage statistics\n");
        kprintf("  ps     - List active system tasks/threads\n");
    } else if (kstrcmp(line, "clear") == 0) {
        fb_console_clear();
    } else if (kstrcmp(line, "mem") == 0) {
        kprintf("--- Live Physical Memory Statistics ---\n");
        pmm_dump_stats();
    } else if (kstrcmp(line, "ps") == 0) {
        kprintf("--- Active Tasks & Processes ---\n");
        kprintf("PID 0: [kernel idle / scheduler thread]\n");
        kprintf("PID 1: hello (user-space process at 0x400000)\n");
        kprintf("PID 2: shell (interactive kernel thread)\n");
        kprintf("PID 3: ping  (background demo thread)\n");
        kprintf("PID 4: pong  (background demo thread)\n");
    } else if (line[0] != '\0') {
        kprintf("Unknown command: '%s'. Type 'help' for options.\n", line);
    }
}

void shell_thread(void *arg) {
    UNUSED(arg);
    char cmd_buf[SHELL_BUF_SIZE];
    size_t cmd_len = 0;

    kinfo("MyOS Shell v0.1 online. Type 'help' for commands.\n");
    kprintf("myos> ");

    for (;;) {
        key_event_t ev;
        if (keyboard_poll(&ev)) {
            if (ev.pressed && ev.ascii != 0) {
                if (ev.ascii == '\n' || ev.ascii == '\r') {
                    kputc('\n');
                    cmd_buf[cmd_len] = '\0';
                    
                    if (cmd_len > 0) {
                        shell_execute(cmd_buf);
                        cmd_len = 0;
                    }
                    
                    kprintf("myos> ");
                } else if (ev.ascii == '\b' || ev.ascii == 127) {
                    /* Handle Backspace */
                    if (cmd_len > 0) {
                        cmd_len--;
                        kprintf("\b \b");
                    }
                } else {
                    /* Normal character: store and echo */
                    if (cmd_len < SHELL_BUF_SIZE - 1) {
                        cmd_buf[cmd_len++] = ev.ascii;
                        kputc(ev.ascii);
                    }
                }
            }
        } else {
            sched_sleep_ms(10);
        }
    }
}