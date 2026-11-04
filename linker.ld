OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(kernel_main)

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0xffffffff80000000;

    .text : ALIGN(0x1000)
    {
        *(.text)
        *(.text.*)
    } :text

    .rodata : ALIGN(0x1000)
    {
        *(.rodata)
        *(.rodata.*)
    } :rodata

    .limine_requests : ALIGN(8)
    {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :rodata

    .data : ALIGN(0x1000)
    {
        *(.data)
        *(.data.*)
    } :data

    .bss : ALIGN(0x1000)
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    } :data

    /DISCARD/ :
    {
        *(.eh_frame*)
        *(.note*)
        *(.comment*)
    }
}
