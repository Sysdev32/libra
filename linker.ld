ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS((1 << 2) | (1 << 0)); /* Read | Execute */
    rodata PT_LOAD FLAGS((1 << 2));            /* Read Only */
    data   PT_LOAD FLAGS((1 << 2) | (1 << 1)); /* Read | Write */
}

SECTIONS
{
    . = 0xffffffff80000000;

    /* 1. Executable Code Segment */
    .text : {
        *(.text .text.*)
    } :text
    
    . = ALIGN(0x1000);

    /* 2. Trampoline (Embedded as data in the High-Half) */
    .trampoline : {
        _binary_trampoline_bin_start = .;
        *(.trampoline)
        _binary_trampoline_bin_end = .;
    } :text

    . = ALIGN(0x1000);

    /* 3. Read-Only Data */
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(0x1000);

    /* 4. Data and BSS */
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :data

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
    }
}