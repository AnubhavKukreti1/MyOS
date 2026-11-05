/*
 * MyOS - User Space Demo Program
 * Runs in Ring 3 and communicates with the kernel via int 0x80.
 */

    .section .rodata
    .balign 16
msg:
    .ascii "Hello from Ring 3 via int 0x80!\n"
    .set msg_len, . - msg

    .section .text
    .balign 16
    .global user_demo_start
    .global user_demo_entry
    .global user_demo_end

user_demo_start:
user_demo_entry:
1:  movq  $1, %rax              /* SYS_WRITE */
    movq  $1, %rdi              /* fd = stdout */
    leaq  msg(%rip), %rsi
    movq  $msg_len, %rdx
    int   $0x80

    movq  $4, %rax              /* SYS_SLEEP_MS */
    movq  $1000, %rdi
    int   $0x80
    jmp   1b

user_demo_end:

    .section .note.GNU-stack, "", @progbits