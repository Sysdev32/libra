// SPDX-License-Identifier: GPL-3.0-only
#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// Defines a single 8-byte entry in the Global Descriptor Table
struct GDTEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

// Defines the pointer structure loaded into the CPU's GDTR register
struct GDTPtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// Initializes and loads the custom system GDT
void gdt_init(void);

// Debug helper: dumps GDTR, first GDT qwords, TR and key TSS fields with a tag
void dump_gdt_state(const char* tag);

#endif
