#include <arch/x86_64/idt.h>
#include <string.h>
#include <errno.h>
#include <drivers/fb.h>
#include <drivers/alloc.h>
#include <limine.h>
#include <arch/x86_64/schedule.h>
#include <drivers/hvfs.h>
#include <fs/vfs.h>
#include <uacpi/uacpi.h>
#include <uacpi/sleep.h>
#include <stdint.h>
#include <drivers/net/HTTP.h>
#include <drivers/net/IPV4.h>
#include <drivers/net/TCP.h>
#include <drivers/net/UDP.h>
#include <fs/mnt.h>
#include <hals/net/RTL8139.h>
#include <systable.h>
uintptr_t ioapic_virtual_base = 0;
uintptr_t lapic_virtual_base = 0;
extern volatile struct limine_hhdm_request hhdm_request;
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
#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1
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
    
    // Read the version register to verify communication
    uint32_t version_reg = ioapic_read(ioapic_virtual_base, 0x01);
    int max_entries = ((version_reg >> 16) & 0xFF) + 1;

    // Mask (disable) all redirection entries by default for safety
    for (int i = 0; i < max_entries; i++) {
        uint8_t reg_low = 0x10 + (i * 2);
        uint8_t reg_high = reg_low + 1;

        // Bit 16 = 1 masks the interrupt line
        ioapic_write(ioapic_virtual_base, reg_low, 0x00010000);
        ioapic_write(ioapic_virtual_base, reg_high, 0x00000000);
    }
    
}

void lapic_init(uint32_t physical_address) {
    lapic_virtual_base = (uintptr_t)physical_address + hhdm_request.response->offset;
}

void lapic_eoi(void) {
    if (lapic_virtual_base == 0) return;
    
    volatile uint32_t *eoi_reg = (volatile uint32_t*)(lapic_virtual_base + 0xB0);
    *eoi_reg = 0;
}

void parse_madt(struct acpi_table_madt *madt) {
    if (madt == NULL) return;

    printk(LOG_TRACE, "Parsing MADT. Local APIC Address: 0x%x\n", madt->local_apic_address);
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
            printk(LOG_ERROR, "MADT parsing error: invalid record length %d\n", record->length);
            break;
        }

        // 4. Check the entry type and cast it to your specific structure
        switch (record->type) {
            case 0: {
                struct madt_local_apic *lapic = (struct madt_local_apic *)record;
                if (lapic->flags & 1) { // Bit 0 = Processor Enabled
                    apic[lapicint++] = *lapic;
                }   
                break;
            }
            case 1: {
                struct madt_io_apic *ioapic = (struct madt_io_apic *)record;
                
                ioapic_init(ioapic->io_apic_address);
                break;
            }
            case 2: {
                struct madt_interrupt_override *override = (struct madt_interrupt_override *)record;
                isa_overrides[override->irq_source].gsi = override->gsi;
                break;
            }
            case 5: {
                struct madt_local_apic_override *lapic_64 = (struct madt_local_apic_override *)record;
                printk(LOG_TRACE, "64-bit Local APIC Address Override: 0x%llx\n", lapic_64->local_apic_address_64);
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
void pic_disable(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);  
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
    ioapic_set_entry(ioapic_virtual_base, isa_overrides[1].gsi, 0x21, 0);        // Keyboard IRQ1
    ioapic_set_entry(ioapic_virtual_base, isa_overrides[12].gsi, 0x2C, 0);       // Mouse IRQ12
}
