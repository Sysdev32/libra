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

    /* 4. Uninitialized Data Segment - naturally follows writable data */
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    /* Align to a standard 4KB CPU memory page boundary before initializing the heap */
    . = ALIGN(4096);

    /* Export standard runtime control boundary symbols for the stubs.c _sbrk implementation */
    _heap_start = .;

    /* Advance the tracking allocation offset pointer to cleanly reserve 32MB of User Heap space */
    _heap_end = _heap_start + 0x2000000; 
}