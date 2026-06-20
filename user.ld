ENTRY(_start)

SECTIONS
{
    /* Standard base load address for isolated Ring 3 User execution blocks */
    . = 0x2000000;

    /* 1. Executable Code Segment */
    .text : {
        *(.text._start)     /* Guarantee entry execution initialization */
        *(.text)
        *(.text.*)
    }

    /* Force Read-Only data onto a clean 4KB page boundary */
    . = ALIGN(4096);

    /* 2. Read-Only Data Segment */
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    /* Force Writable data onto a clean 4KB page boundary */
    . = ALIGN(4096);

    /* 3. Writable Data Segment */
    .data : {
        *(.data)
        *(.data.*)
    }

    /* 4. Uninitialized Data Segment */
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    /* Align to a standard 4KB CPU memory page boundary */
    . = ALIGN(4096);

    _heap_start = .;
}

/* Move this definition OUTSIDE of the SECTIONS block so it remains a pure reference pointer */
_heap_end = _heap_start + 0x2000000;