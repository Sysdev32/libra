// SPDX-License-Identifier: GPL-3.0-only
#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// Structure mapping the exact layout pushed to the stack by your updated assembly stubs
struct InterruptRegisters {
    // 1. General-purpose registers (Saved by common_stub)
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    // 3. Macro stub registers ordered exactly by stack pop trajectory
    uint64_t int_no;     // Pushed last by macro stub (lowest address slot)
    uint64_t error_code; // Pushed first by macro stub (highest address slot)

    // 4. Hardware IRETQ frame automatically provided by the CPU execution logic
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

// A single 16-byte gate descriptor entry in x86_64 long-mode IDT
struct IDTEntry {
    uint16_t isr_low;
    uint16_t kernel_cs;
    uint8_t  ist;
    uint8_t  attributes;
    uint16_t isr_mid;
    uint32_t isr_high;
    uint32_t reserved;
} __attribute__((packed));

struct IDTPtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#pragma pack(push, 1)

// Standard ACPI Table Header
struct acpi_header {
    char     signature[4];     // 'APIC'
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

// The base MADT structure directly following the signature
struct acpi_table_madt {
    struct acpi_header header;
    uint32_t local_apic_address; // 32-bit physical address of Local APICs
    uint32_t flags;              // Bit 0 = Dual 8259 Legacy PICs Installed
};

// The generic record header found at the start of every variable length entry
struct madt_record_header {
    uint8_t type;
    uint8_t length;
};

#pragma pack(pop)

// Entry Type 0: Processor Local APIC
struct madt_local_apic {
    uint8_t  type;             // 0
    uint8_t  length;           // 8
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;            // Bit 0 = Enabled, Bit 1 = Online Capable
};

// Entry Type 1: I/O APIC
struct madt_io_apic {
    uint8_t  type;             // 1
    uint8_t  length;           // 12
    uint8_t  io_apic_id;
    uint8_t  reserved;
    uint32_t io_apic_address;  // Physical memory-mapped address
    uint32_t gsi_base;         // Global System Interrupt Base
};

// Entry Type 2: I/O APIC Interrupt Source Override
struct madt_interrupt_override {
    uint8_t  type;             // 2
    uint8_t  length;           // 10
    uint8_t  bus_source;       // Usually 0 for ISA
    uint8_t  irq_source;       // Source IRQ (e.g. 0 for timer)
    uint32_t gsi;              // What GSI it maps to (e.g. 2)
    uint16_t flags;            // Polarity and Trigger Mode flags
};

// Entry Type 3: I/O APIC Non-maskable interrupt source
struct madt_io_apic_nmi {
    uint8_t  type;             // 3
    uint8_t  length;           // 8
    uint8_t  nmi_source;
    uint8_t  reserved;
    uint16_t flags;
    uint32_t gsi;
};

// Entry Type 4: Local APIC Non-maskable interrupts
struct madt_local_apic_nmi {
    uint8_t  type;             // 4
    uint8_t  length;           // 6
    uint8_t  acpi_processor_id;// 0xFF means all processors
    uint16_t flags;
    uint8_t  lint;             // LINT0 or LINT1
};

// Entry Type 5: Local APIC Address Override
struct madt_local_apic_override {
    uint8_t  type;             // 5
    uint8_t  length;           // 12
    uint16_t reserved;
    uint64_t local_apic_address_64; // Use this 64-bit pointer if present!
};

// Entry Type 9: Processor Local x2APIC
struct madt_local_x2apic {
    uint8_t  type;             // 9
    uint8_t  length;           // 16
    uint16_t reserved;
    uint32_t x2apic_id;
    uint32_t flags;            // Same flags as Type 0
    uint32_t acpi_id;
};

typedef struct {
    uint8_t arg[8];
} arg;
void sleep_ms(uint64_t ms);
void parse_madt(struct acpi_table_madt *madt);
void idt_init(void);
void ioapic(struct acpi_table_madt*);
void outb(uint16_t port, uint8_t val);
uint64_t exception_handler_c(struct InterruptRegisters *regs);
uint64_t intrhandler(struct InterruptRegisters *regs);
void lapic_eoi(void);

#endif