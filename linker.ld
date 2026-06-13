ENTRY(_start)

PHDRS
{
    text    PT_LOAD FLAGS((1 << 2) | (1 << 0)); /* Read | Execute */
    rodata  PT_LOAD FLAGS((1 << 2));             /* Read Only */
    data    PT_LOAD FLAGS((1 << 2) | (1 << 1)); /* Read | Write */
}

SECTIONS
{
    /* Start in the higher-half memory space */
    . = 0xffffffff80000000;

    /* 1. Executable Code Segment */
    .text : {
        *(.text .text.*)
    } :text

    /* Force the next segment onto a completely fresh 4KB page boundary */
    . = ALIGN(0x1000);

    /* 2. Read-Only Data Segment */
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    /* Force another page alignment to separate read-only from writable data */
    . = ALIGN(0x1000);

    /* 3. Limine Handshaking Blocks */
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :data

    /* 4. Standard Read-Write Data Segment */
    .data : {
        *(.data .data.*)
    } :data

    /* 5. Uninitialized Data Segment */
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /* Discard unnecessary metadata blocks */
    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
    }
}