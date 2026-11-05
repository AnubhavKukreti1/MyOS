/*
 * MyOS - src/arch/x86_64/isr_stubs.S
 * 256 entry stubs. exception_dispatch() returns the frame to resume, which
 * is how the scheduler switches tasks. Frame layout = interrupt_frame_t.
 */

    .pushsection .rodata.isr_tbl, "a"
    .balign 8
    .global isr_stub_table
isr_stub_table:
    .popsection

    .altmacro
    .macro ISR_STUB vec
    .pushsection .text
    .balign 16
isr_stub_\vec:
    .if (\vec == 8) || ((\vec >= 10) && (\vec <= 14)) || (\vec == 17) || (\vec == 21) || (\vec == 29) || (\vec == 30)
    .else
    pushq $0                    /* dummy error code */
    .endif
    pushq $\vec
    jmp isr_common
    .popsection
    .pushsection .rodata.isr_tbl, "a"
    .quad isr_stub_\vec
    .popsection
    .endm

    .set vec_i, 0
    .rept 256
    ISR_STUB %vec_i
    .set vec_i, vec_i + 1
    .endr

    .text
isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq  %rsp, %rdi            /* arg0: interrupt_frame_t * */
    cld
    call  exception_dispatch
    movq  %rax, %rsp            /* resume the frame it returned (may be another task's) */

    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %r11
    popq  %r10
    popq  %r9
    popq  %r8
    popq  %rbp
    popq  %rdi
    popq  %rsi
    popq  %rdx
    popq  %rcx
    popq  %rbx
    popq  %rax

    addq  $16, %rsp             /* drop vector + error code */
    iretq

    .section .note.GNU-stack, "", @progbits