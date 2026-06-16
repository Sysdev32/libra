// SPDX-License-Identifier: GPL-3.0-only
#include <drivers/idt.h>
#include <string.h>
#include <drivers/fb.h>
#include <limine.h>
#include <drivers/schedule.h>
#include <drivers/vfs.h>
#include <uacpi/uacpi.h>
#include <uacpi/sleep.h>
typedef void (*interrupt)(struct InterruptRegisters *regs);

typedef struct {
    interrupt intr;
    int vector;
} handle;

// --- GLOBAL VARIABLES AND CONFIGURATION TRACKING ---
handle handles[512] = {};
#define MAX_OVERRIDES 32

struct interrupt_override {
    uint8_t irq_source;
    uint32_t gsi;
    uint16_t flags;
};

struct interrupt_override isa_overrides[MAX_OVERRIDES];
int override_count = 0;
struct madt_local_apic apic[32];
int lapicint;
extern volatile struct limine_hhdm_request hhdm_request;
uintptr_t ioapic_virtual_base = 0;
uintptr_t lapic_virtual_base = 0;

#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1

extern void idt_load(struct IDTPtr *ptr);
extern void intr(void);
extern void *pmm_alloc_pages(int order);
extern uint64_t exception_vector_table[34]; // Expanded to hold all 34 elements (0-33)

static struct IDTEntry idt[256];
static struct IDTPtr   idt_ptr;

// --- CORE IOAPIC HARDWARE WINDOW INTERFACE ---

// 1. Core hardware write function using the index/data window
void ioapic_write(uintptr_t base, uint8_t reg_index, uint32_t value) {
    volatile uint32_t *regsel = (volatile uint32_t*)(base + 0x00);
    volatile uint32_t *iowin  = (volatile uint32_t*)(base + 0x10);

    *regsel = reg_index;
    *iowin = value;
}

// 2. Core hardware read function using the index/data window
uint32_t ioapic_read(uintptr_t base, uint8_t reg_index) {
    volatile uint32_t *regsel = (volatile uint32_t*)(base + 0x00);
    volatile uint32_t *iowin  = (volatile uint32_t*)(base + 0x10);

    *regsel = reg_index;
    return *iowin;
}

// 3. Redirection Table Entry setter function to map pins to IDT vectors
void ioapic_set_entry(uintptr_t base, uint8_t pin, uint8_t idt_vector, uint8_t target_apic_id) {
    uint8_t reg_low = 0x10 + (pin * 2);
    uint8_t reg_high = reg_low + 1;

    // High 32 bits: Put target local APIC ID in bits 24-31
    // Fixed to target direct physical ID to prevent multi-core execution stalls
    uint32_t value_high = (uint32_t)target_apic_id << 24;

    // Low 32 bits: Start with IDT Vector number (bits 0-7)
    // Bit 16 is 0 (unmasked/enabled)
    // Delivery mode is 000 (fixed)
    // Trigger mode is 0 (edge-triggered for ISA)
    uint32_t value_low = idt_vector;

    ioapic_write(base, reg_high, value_high);
    ioapic_write(base, reg_low, value_low);
}

// 4. Call this function when you parse Entry Type 1 in your MADT loop
void ioapic_init(uint32_t physical_address) {
    // Calculate virtual address using Limine's Higher-Half Direct Map offset
    ioapic_virtual_base = (uintptr_t)physical_address + hhdm_request.response->offset;
    
    printk("IOAPIC mapped at virtual address: %p\n", (void*)ioapic_virtual_base);

    // Read the version register to verify communication
    uint32_t version_reg = ioapic_read(ioapic_virtual_base, 0x01);
    int max_entries = ((version_reg >> 16) & 0xFF) + 1;

    printk("IOAPIC verified. Max Redirection Entries: %d\n", max_entries);

    // Mask (disable) all redirection entries by default for safety
    for (int i = 0; i < max_entries; i++) {
        uint8_t reg_low = 0x10 + (i * 2);
        uint8_t reg_high = reg_low + 1;

        // Bit 16 = 1 masks the interrupt line
        ioapic_write(ioapic_virtual_base, reg_low, 0x00010000);
        ioapic_write(ioapic_virtual_base, reg_high, 0x00000000);
    }
    
    printk("All IOAPIC pins masked and ready for manual routing.\n");
}

void lapic_init(uint32_t physical_address) {
    lapic_virtual_base = (uintptr_t)physical_address + hhdm_request.response->offset;
}

// --- ACPI MULTIPLE APIC DESCRIPTION TABLE (MADT) ENGINE ---

void parse_madt(struct acpi_table_madt *madt) {
    if (madt == NULL) return;

    printk("Parsing MADT. Local APIC Address: 0x%x\n", madt->local_apic_address);
    lapic_init(madt->local_apic_address);
    
    // 1. Find where the variable-length records begin
    // Skip the main header to land exactly at offset 0x2C
    uintptr_t current_addr = (uintptr_t)madt + sizeof(struct acpi_table_madt);
    
    // 2. Calculate exactly where the records end using the table's total length
    uintptr_t end_addr = (uintptr_t)madt + madt->header.length;
    for (int i = 0; i < 32; i++) {
        isa_overrides[i].irq_source = i;
        isa_overrides[i].gsi = i;
    }

    // 3. Loop through the records safely
    while (current_addr < end_addr) {
        struct madt_record_header *record = (struct madt_record_header *)current_addr;

        // Malformed table safety check: length cannot be 0 or push us past the end
        if (record->length == 0 || (current_addr + record->length) > end_addr) {
            printk("MADT parsing error: invalid record length %d\n", record->length);
            break;
        }

        // 4. Check the entry type and cast it to your specific structure
        switch (record->type) {
            case 0: {
                struct madt_local_apic *lapic = (struct madt_local_apic *)record;
                if (lapic->flags & 1) { // Bit 0 = Processor Enabled
                    printk("Found CPU Core - Processor ID: %d, APIC ID: %d\n", 
                           lapic->acpi_processor_id, lapic->apic_id);
                    apic[lapicint++] = *lapic;
                }   
                break;
            }
            case 1: {
                struct madt_io_apic *ioapic = (struct madt_io_apic *)record;
                printk("Found I/O APIC - ID: %d, Address: 0x%x, GSI Base: %d\n",
                       ioapic->io_apic_id, ioapic->io_apic_address, ioapic->gsi_base);
                ioapic_init(ioapic->io_apic_address);
                break;
            }
            case 2: {
                struct madt_interrupt_override *override = (struct madt_interrupt_override *)record;
                printk("Interrupt Override - IRQ Source %d -> GSI %d\n",
                       override->irq_source, override->gsi);
                isa_overrides[override->irq_source].gsi = override->gsi;
                break;
            }
            case 5: {
                struct madt_local_apic_override *lapic_64 = (struct madt_local_apic_override *)record;
                printk("64-bit Local APIC Address Override: 0x%llx\n", lapic_64->local_apic_address_64);
                break;
            }
            default:
                // Types we don't care about right now (like NMIs) are skipped safely
                break;
        }

        // 5. Jump directly to the next record header using its precise length
        current_addr += record->length;
    }
}

// --- HARDWARE INTERACTION PORT WRAPPERS ---

void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void pic_disable(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);  
}

// --- IDT MANIPULATION DESCRIPTOR SUITE ---

static void idt_set_descriptor(uint8_t vector, void *isr, uint8_t attributes) {
    uint64_t addr = (uint64_t)isr;
    idt[vector].isr_low    = (uint16_t)(addr & 0xFFFF);
    idt[vector].kernel_cs  = 0x08; 
    idt[vector].ist        = 0;
    idt[vector].attributes = attributes;
    idt[vector].isr_mid    = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vector].isr_high   = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vector].reserved   = 0;
}

char* crop(char *dest, const char *src, int until) {
    for (int i = 0; i < until-1; i++) {
        dest[i] = src[i];
    }
    dest[until-1] = '\0';
    return dest; // Returns the pointer to the destination buffer
}

void idt_init(void) {
    memset(idt, 0, sizeof(struct IDTEntry) * 256);

    // 1. Loop exactly 32 times to map exception_vector_table[0] through [31]
    for (int i = 0; i < 32; i++) {
        idt_set_descriptor(i, (void*)exception_vector_table[i], 0x8E);
    }

    // Ensure Double Fault (#DF, vector 8) uses IST entry 1 to guarantee
    // a dedicated emergency stack (prevents triple-faults if the normal
    // kernel stack is corrupted).
    idt[8].ist = 1;

    // 2. Safely map vector 0x20 and 0x21 using slots 32 and 33 from the assembly landing pads
    idt_set_descriptor(0x20, (void*)exception_vector_table[32], 0x8E);
    idt_set_descriptor(0x21, (void*)exception_vector_table[33], 0x8E);
    idt_set_descriptor(0x80, intr, 0xEE); // User Mode System Calls Gate

    // 3. Load the IDT pointer into the processor
    idt_ptr.limit = (sizeof(struct IDTEntry) * 256) - 1;
    idt_ptr.base  = (uint64_t)&idt;

    idt_load(&idt_ptr);

    // Debug: read back IDTR to ensure it was loaded correctly
    struct IDTPtr cur_idt;
    asm volatile("sidt %0" : "=m"(cur_idt));
    printk("[DBG] IDTR after lidt -> base=%p limit=0x%x\n", (void*)cur_idt.base, cur_idt.limit);

    // 4. Disable legacy PIC
    pic_disable();
}

void lapic_eoi(void) {
    if (lapic_virtual_base == 0) return;
    
    volatile uint32_t *eoi_reg = (volatile uint32_t*)(lapic_virtual_base + 0xB0);
    *eoi_reg = 0;
}

void timer() {
    printk("huh");
    lapic_eoi();
}

void ioapic(struct acpi_table_madt* madt) {
    // 1. Disable the old 8259 PIC completely
    pic_disable();
    
    // 2. Parse MADT to find lapic_virtual_base and ioapic_virtual_base
    parse_madt(madt);
    
    // 3. Turn on the local APIC software enable flag
    if (lapic_virtual_base != 0) {
        volatile uint32_t *spurious_reg = (volatile uint32_t*)(lapic_virtual_base + 0xF0);
        // (1 << 8) sets the Software Enable bit to 1
        // 0xFF maps the spurious vector to index 255 in the IDT
        *spurious_reg = 0xFF | (1 << 8); 
    }

    // 4. Route ISA IRQs safely to IDT vectors via the IOAPIC targeting Core 0
    ioapic_set_entry(ioapic_virtual_base, 2, 0x20, 0x00);                       // PIT route mapping
    ioapic_set_entry(ioapic_virtual_base, isa_overrides[1].gsi, 0x21, 0x00);    // Keyboard route mapping

    // 5. Initialize the scheduler structures (Sets up Task 0 as RUNNING)
    init_scheduler();
}

// --- VIRTUAL FILE SYSTEM SYSTEM-CALL ISOLATED ROUTER ---

static void handle_syscall(struct InterruptRegisters *regs) {
    arg *args = (arg*)regs->rbx;

    switch (regs->rax) {
        case 0: // vfs_read
            if (args->arg <= 2) {
                regs->rax = 0; 
            } else {
                regs->rax = vfs_read(args->arg[0] - 2, (void*)args->arg[1], args->arg[2], args->arg[3]);
            }
            break;

        case 1: // vfs_write_file
            if (args->arg > 2) {
                regs->rax = vfs_write_file(args->arg[0] - 2, (void*)args->arg[1], args->arg[2]);
            } 
            else if (args->arg == 1 || args->arg == 2) {
                char *user_str = (char*)args->arg[0];
                unsigned long length = args->arg[1];

                for (unsigned long i = 0; i < length; i++) {
                    printk("%c", user_str[i]);
                }
                regs->rax = length; 
            }
            break;

        case 2: { // vfs_open
            long fd = vfs_open((const char*)args->arg[0]);
            if (fd < 0) {
                regs->rax = fd; 
            } else {
                regs->rax = fd + 2; 
            }
            break;
        }

        case 3: // vfs_mkdir
            regs->rax = vfs_mkdir((const char*)args->arg[0], args->arg[1]);
            break;

        case 4: // vfs_rmdir
            regs->rax = vfs_rmdir((const char*)args->arg[0]);
            break;

        case 5: // vfs_free_fd (_close)
            if (args->arg <= 2) {
                regs->rax = 0; 
            } else {
                regs->rax = vfs_free_fd(args->arg[0] - 2);
            }
            break;

        case 6: // vfs_move_file
            if (args->arg <= 2) {
                regs->rax = -1; 
            } else {
                regs->rax = vfs_move_file(args->arg[0] - 2, (const char*)args->arg[1]);
            }
            break;

        case 7: { // vfs_create_file
            long fd = vfs_create_file((void*)args->arg[0], (const char*)args->arg[1], args->arg[2]);
            if (fd < 0) {
                regs->rax = fd; 
            } else {
                regs->rax = fd + 2; 
            }
            break;
        }

        case 8: // vfs_delete_file
            regs->rax = vfs_delete_file((const char*)args->arg[0]);
            break;

        default: 
            regs->rax = -1; // Unknown syscall ID
            break;
    }
}

// --- COMMON HARDWARE AND LEGACY INTERRUPT DISPATCH ROUTER ---
volatile uint64_t ticks = 0;
uint64_t intrhandler(struct InterruptRegisters* regs) {
    uint64_t vector = regs->int_no;

    // --- DISPATCH GATE A: SYSTEM CALL SYSTEM GATE (Vector 0x80 / 128) ---
    if (vector == 128 || vector == 0x80) {
        handle_syscall(regs);
        return 0;
    }

    // --- DISPATCH GATE B: SYSTEM PREEMPTIVE TIMER INTERRUPT (Vector 32) ---
    if (vector == 0) {
        ticks += 1;
        uint64_t new_rsp = schedule_preemptive((uint64_t)regs);
        lapic_eoi();
        return new_rsp;
    }
    for (int i = 0; i < 512; i++) {
        if ((uint64_t)handles[i].vector == vector && handles[i].intr != NULL) {
            // Call the registered device driver callback function
            handles[i].intr(regs);
            break;
        }
    }

    lapic_eoi();
    return 0;
}
// --- ARCHITECTURAL EXCEPTION NAMES STORAGE ENGINE ---

static const char *exception_names[] = {
    "Divide-by-Zero (#DE)",                  // 0
    "Debug (#DB)",                           // 1
    "Non-Maskable Interrupt (NMI)",         // 2
    "Breakpoint (#BP)",                     // 3
    "Overflow (#OF)",                        // 4
    "Bound Range Exceeded (#BR)",            // 5
    "Invalid Opcode (#UD)",                  // 6
    "Device Not Available (#NM)",            // 7
    "Double Fault (#DF)",                    // 8
    "Coprocessor Segment Overrun",           // 9
    "Invalid TSS (#TS)",                     // 10
    "Segment Not Present (#NP)",             // 11
    "Stack-Segment Fault (#SS)",             // 12
    "General Protection Fault (#GP)",        // 13
    "Page Fault (#PF)",                      // 14
    "Reserved / Unknown",                    // 15
    "x87 Floating-Point Exception (#MF)",    // 16
    "Alignment Check (#AC)",                 // 17
    "Machine Check (#MC)",                   // 18
    "SIMD Floating-Point Exception (#XM)",   // 19
    "Virtualization Exception (#VE)",        // 20
    "Control Protection Exception (#CP)",    // 21
    "Reserved", "Reserved", "Reserved",     // 22-24
    "Reserved", "Reserved", "Reserved",     // 25-27
    "Hypervisor Injection Exception (#HV)",  // 28
    "VMM Communication Exception (#VC)",     // 29
    "Security Exception (#SE)",              // 30
    "Reserved"                               // 31
};

// --- CORE CPU EXCEPTION DIAGNOSTICS AND CRASH DUMP ANALYSIS ENGINE ---
void sleep_ms(uint64_t ms)
{
    uint64_t tickst = ms / 10;

    uint64_t start = ticks;

    while ((ticks - start) < tickst) {

        __asm__ volatile("hlt"); // low power wait until next interrupt
    }
}
char *strcpy(char *dest, const char *src)
{
    char *orig = dest;

    while (*src) {
        *dest++ = *src++;
    }

    *dest = '\0';

    return orig;
}
uint64_t exception_handler_c(struct InterruptRegisters *regs) {
    asm volatile ("sti");
    uint64_t vector = regs->int_no; 

    // --- CASE A: CRITICAL ARCHITECTURAL CPU EXCEPTIONS (0-31) ---
    if (vector < 32) {
        char buffer[64];
        for (int i = 0; i < 64; i++) buffer[i] = 0; // Clear the buffer for safety
        if (vector > 255) {
            strcpy(buffer, "(invalid)");
        }
        
        printk("Vector Index    : %d (0x%x) %s\r\n", vector, vector, buffer);
        printk("Description     : %s\r\n", exception_names[vector]);
        printk("Error Code Mask : 0x%x\r\n", regs->error_code);
        printk("--------------------------------------------------\r\n");
        
        // Output code boundaries and stack tracking snapshots
        printk("Faulting Instruction Pointer (RIP): %p\r\n", regs->rip);
        printk("Faulting Stack Pointer       (RSP): %p\r\n", regs->rsp);
        printk("Code Segment Selector        (CS) : %x\r\n", regs->cs);
        printk("Processor Flag Mask      (RFLAGS) : %p\r\n", regs->rflags);
        
        // Specialize tracking read metrics for standard complex traps
        switch (vector) {
            case 13: { // General Protection Fault
                printk(" -> Details: Memory segment selection or boundary access rule violation.\r\n");
                if (regs->error_code != 0) {
                    printk(" -> Broken Selector Target Segment Index: GDT/LDT Slot %d\r\n", regs->error_code >> 3);
                }
                break;
            }
            case 14: { // Page Fault
                uint64_t faulting_address = 0;
                asm volatile("mov %%cr2, %0" : "=r"(faulting_address));
                printk(" -> Details: Attempted to touch unmapped or protected virtual address layout space.\r\n");
                printk(" -> Missing Destination Target (CR2): 0x%p\r\n", faulting_address);
                
                // Decode page fault reason flags out of error code bitmask
                printk(" -> Fault Parameters: %s, %s, %s\r\n",
                    (regs->error_code & (1 << 0)) ? "Protection Violation" : "Non-Present Page",
                    (regs->error_code & (1 << 1)) ? "Write Operation" : "Read Operation",
                    (regs->error_code & (1 << 2)) ? "User Privilege Level" : "Supervisor Ring 0 Code"
                );
                break;
            }
            default:
                printk(" -> Details: Fatal core execution anomaly, stalling CPU state pipeline layout.\r\n");
                break;
        }

        printk("--------------------------------------------------\r\n");
        printk("Register Dump:\r\n");
        printk("RAX: %p  RBX: %p  RCX: %p  RDX: %p\r\n", regs->rax, regs->rbx, regs->rcx, regs->rdx);
        printk("RSI: %p  RDI: %p  RBP: %p  R8 : %p\r\n", regs->rsi, regs->rdi, regs->rbp, regs->r8);
        printk("R9 : %p  R10: %p  R11: %p  R12: %p\r\n", regs->r9, regs->r10, regs->r11, regs->r12);
        printk("R13: %p  R14: %p  R15: %p\r\n", regs->r13, regs->r14, regs->r15);
        printk("==================================================\r\n");
        printk("Rebooting in 5 seconds...\r\n");
        sleep_ms(1000);
        printk("Rebooting in 4 seconds...\r\n");
        sleep_ms(1000);
        printk("Rebooting in 3 seconds...\r\n");
        sleep_ms(1000);
        printk("Rebooting in 2 seconds...\r\n");
        sleep_ms(1000);
        printk("Rebooting in 1 seconds...\r\n");
        sleep_ms(1000);
        
        uacpi_reboot();
        // Force definitive crash freeze so hardware doesn't pass broken state steps downward
        for (;;) {
            asm volatile("hlt");
        }
    } 
    
    // --- CASE B: DYNAMIC INTERRUPT FALLBACK ROUTING PATH ---
    else {
        if (vector == 0x20) {
            uint64_t new_rsp = schedule_preemptive((uint64_t)regs);
            lapic_eoi();
            return new_rsp; 
        } else {
            for (int i = 0; i < 512; i++) {
                if ((uint64_t)handles[i].vector == vector && handles[i].intr != NULL) {
                    handles[i].intr(regs);
                    break;
                }
            }
            lapic_eoi();
        }
    }

    return 0;
}