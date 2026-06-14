// SPDX-License-Identifier: GPL-3.0-only
#include <drivers/gdt.h>
#include <drivers/fb.h>
#include <string.h> // For memset

// Allocate space for 7 entries:
// 5 standard entries (Null, KCode, KData, UCode, UData) + 2 slots for the 16-byte TSS descriptor
// Align the GDT and TSS to a page boundary to reduce accidental overwrites and
// ensure stable physical/virtual placement when identity mapping changes.
static struct GDTEntry gdt[7] __attribute__((aligned(4096)));
static struct GDTPtr   gdt_ptr __attribute__((aligned(8)));

// The global hardware TSS structure
struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;         // The trusted Kernel Stack Pointer loaded during Ring 3 -> Ring 0 jumps
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;         // Interrupt Stack Table pointers (unused for now)
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

struct tss_entry global_tss __attribute__((aligned(4096)));

// Dedicated emergency stack for handling user-space interrupts inside the kernel
static uint8_t bsp_kernel_stack[4096];

// Standard helper function for basic 8-byte entries
static void gdt_set_gate(int num, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = 0;
    gdt[num].base_middle = 0;
    gdt[num].base_high   = 0;
    gdt[num].limit_low   = 0;
    gdt[num].access      = access;
    gdt[num].granularity = gran;
}

// Special helper to write a 16-byte TSS descriptor split across two sequential GDT slots
static void gdt_set_tss(int num, uint64_t base, uint32_t limit) {
    // Build a proper 16-byte (128-bit) TSS descriptor according to Intel manual
    // Low 64 bits layout:
    //  bits 0-15   : limit[15:0]
    //  bits 16-31  : base[15:0]
    //  bits 32-39  : base[23:16]
    //  bits 40-47  : access
    //  bits 48-51  : limit[19:16]
    //  bits 52-55  : flags (granularity)
    //  bits 56-63  : base[31:24]
    // High 64 bits layout:
    //  bits 0-31   : base[63:32]
    //  bits 32-63  : zero

    uint64_t low = 0;
    uint64_t high = 0;

    low |= (uint64_t)(limit & 0xFFFF);                    // limit[15:0]
    low |= (uint64_t)(base & 0xFFFF) << 16;               // base[15:0]
    low |= (uint64_t)((base >> 16) & 0xFF) << 32;         // base[23:16]
    low |= (uint64_t)(0x89) << 40;                        // access: present + type (available 64-bit TSS)
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;         // limit[19:16]
    low |= (uint64_t)(0x0) << 52;                         // flags/granularity = 0 for TSS
    low |= (uint64_t)((base >> 24) & 0xFF) << 56;         // base[31:24]

    high |= (uint64_t)((base >> 32) & 0xFFFFFFFF);       // base[63:32]

    // Store the two 8-byte chunks into the GDT slots (slot `num` and `num+1`)
    *((uint64_t*)&gdt[num])     = low;
    *((uint64_t*)&gdt[num + 1]) = high;
}

void gdt_init(void) {
    // Entry 0: Null Descriptor
    gdt_set_gate(0, 0, 0);

    // Entry 1: Kernel Code Segment (Selector 0x08)
    gdt_set_gate(1, 0x9A, 0x20);

    // Entry 2: Kernel Data Segment (Selector 0x10)
    gdt_set_gate(2, 0x92, 0x00);

    // Entry 3: User Code Segment (Selector 0x1B)
    gdt_set_gate(3, 0xFA, 0x20);

    // Entry 4: User Data Segment (Selector 0x23)
    gdt_set_gate(4, 0xF2, 0x00);

    // --- SETUP TASK STATE SEGMENT STRUCTURE ---
    memset(&global_tss, 0, sizeof(struct tss_entry));
    
    // Assign the top of our dedicated array to the trusted RSP0 boundary track
    global_tss.rsp0 = (uint64_t)&bsp_kernel_stack[4096];
    // Populate IST1 with a dedicated emergency stack top (aligned)
    global_tss.ist1 = (uint64_t)((uint64_t)&bsp_kernel_stack[4096] & ~0xFULL);
    global_tss.iopb_offset = sizeof(struct tss_entry);

    // --- WRITE 16-BYTE TSS INTO GDT SLOTS 5 AND 6 ---
    uint64_t tss_base = (uint64_t)&global_tss;
    uint32_t tss_limit = sizeof(struct tss_entry) - 1;
    gdt_set_tss(5, tss_base, tss_limit);

    // Set the pointer targets to feed the processor register (now 7 entries total)
    gdt_ptr.limit = (sizeof(struct GDTEntry) * 7) - 1;
    gdt_ptr.base  = (uint64_t)&gdt;

    // Load GDTR register via Assembly, clear segments, and flash the task register
    asm volatile (
        "lgdt %0\n\t"                      // Load GDT pointer into hardware
        "push $0x08\n\t"                   // Push our Kernel Code Selector (0x08)
        "lea 1f(%%rip), %%rax\n\t"         // Get execution address of label 1
        "push %%rax\n\t"                   // Push target instruction pointer location
        "lretq\n\t"                        // Execute a 64-bit Far Return to flush CS register
        "1:\n\t"                           
        "mov $0x10, %%ax\n\t"              // Load Kernel Data Selector (0x10)
        "mov %%ax, %%ds\n\t"               
        "mov %%ax, %%es\n\t"               
        "mov %%ax, %%fs\n\t"               
        "mov %%ax, %%gs\n\t"               
        "mov %%ax, %%ss\n\t"               
        
        // --- LOAD THE SELECTOR INTO THE TASK REGISTER ---
        // Selector index 5 * 8 bytes per entry = 0x28
        "mov $0x28, %%ax\n\t"              
        "ltr %%ax\n\t"                     // Tells the CPU hardware where the valid RSP0 register lives
        :
        : "m"(gdt_ptr)
        : "rax", "memory"
    );

    // Dump the hardware GDTR/GDT and TSS state immediately after load
    dump_gdt_state("after_gdt_init");

}

// Lightweight runtime dump function to help detect when GDTR/GDT/TSS diverge
void dump_gdt_state(const char* tag) {
    struct GDTPtr hw;
    uint16_t tr_val = 0;

    // Read processor GDTR and TR
    asm volatile("sgdt %0" : "=m"(hw));
    asm volatile("str %0" : "=r"(tr_val));

    printk("%s: GDTR base=%p limit=0x%x\n", tag, (void*)hw.base, hw.limit);
    printk("%s: TR=0x%04x TSS_addr=%p\n", tag, tr_val, (void*)&global_tss);

    // Print first 7 GDT 8-byte entries (as qwords)
    uint64_t *gdt_q = (uint64_t*)hw.base;
    for (int i = 0; i < 7; i++) {
        uint64_t val = 0;
        // protect against invalid memory reads by trying to read, but no crashes expected in-kernel
        val = gdt_q[i];
        printk("%s: GDT[%d]=0x%016llx\n", tag, i, (unsigned long long)val);
    }

    // Print a couple of TSS fields for verification
    printk("%s: TSS.rsp0=%p TSS.ist1=%p iopb=0x%x\n", tag,
           (void*)global_tss.rsp0, (void*)global_tss.ist1, global_tss.iopb_offset);
}