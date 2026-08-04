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

#define MAX_CPUS 256
#define MAX_OVERRIDES 32
#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1

// Register Offsets for Local APIC
#define LAPIC_ID_REG        0x0020
#define LAPIC_EOI_REG       0x00B0
#define LAPIC_SVR_REG       0x00F0
#define LAPIC_ICR_LOW       0x0300
#define LAPIC_ICR_HIGH      0x0310

uintptr_t ioapic_virtual_base = 0;
uintptr_t lapic_virtual_base = 0;
extern volatile struct limine_hhdm_request hhdm_request;

struct interrupt_override {
    uint8_t irq_source;
    uint32_t gsi;
    uint16_t flags;
};

struct interrupt_override isa_overrides[MAX_OVERRIDES];
int override_count = 0;

struct madt_local_apic cpus[MAX_CPUS];
uint32_t cpu_count = 0;
uint32_t bsp_apic_id = 0;

// IOAPIC Direct Memory I/O
void ioapic_write(uintptr_t base, uint8_t reg_index, uint32_t value) {
    volatile uint32_t *regsel = (volatile uint32_t*)(base + 0x00);
    volatile uint32_t *iowin  = (volatile uint32_t*)(base + 0x10);

    *regsel = reg_index;
    *iowin = value;
}

uint32_t ioapic_read(uintptr_t base, uint8_t reg_index) {
    volatile uint32_t *regsel = (volatile uint32_t*)(base + 0x00);
    volatile uint32_t *iowin  = (volatile uint32_t*)(base + 0x10);

    *regsel = reg_index;
    return *iowin;
}

void ioapic_set_entry(uintptr_t base, uint8_t pin, uint8_t idt_vector, uint8_t target_apic_id) {
    uint8_t reg_low = 0x10 + (pin * 2);
    uint8_t reg_high = reg_low + 1;

    uint32_t value_high = (uint32_t)target_apic_id << 24;
    uint32_t value_low = idt_vector; // Delivery mode 0 (Fixed), Mask bit = 0 (Enabled)

    ioapic_write(base, reg_high, value_high);
    ioapic_write(base, reg_low, value_low);
}

void ioapic_init(uint32_t physical_address) {
    ioapic_virtual_base = (uintptr_t)physical_address + hhdm_request.response->offset;

    uint32_t version_reg = ioapic_read(ioapic_virtual_base, 0x01);
    int max_entries = ((version_reg >> 16) & 0xFF) + 1;

    // Mask all entries across the board initially
    for (int i = 0; i < max_entries; i++) {
        uint8_t reg_low = 0x10 + (i * 2);
        uint8_t reg_high = reg_low + 1;

        ioapic_write(ioapic_virtual_base, reg_low, 0x00010000); // Set Bit 16 (Masked)
        ioapic_write(ioapic_virtual_base, reg_high, 0x00000000);
    }
}

// Global base assignment for Local APIC
void lapic_set_base(uint64_t physical_address) {
    lapic_virtual_base = (uintptr_t)physical_address + hhdm_request.response->offset;
}

// Enable local LAPIC (Called per core: BSP during boot, APs inside init_ap_scheduler)
void lapic_enable(void) {
    if (lapic_virtual_base == 0) return;

    // Software enable LAPIC (Bit 8) and map spurious vector to IDT 0xFF
    volatile uint32_t *spurious_reg = (volatile uint32_t*)(lapic_virtual_base + LAPIC_SVR_REG);
    *spurious_reg = 0xFF | (1 << 8);
}

// Legacy alias wrapper for lapic_enable
void lapic_init_core(void) {
    lapic_enable();
}

uint32_t lapic_get_id(void) {
    if (lapic_virtual_base == 0) return 0;
    volatile uint32_t *id_reg = (volatile uint32_t*)(lapic_virtual_base + LAPIC_ID_REG);
    return (*id_reg) >> 24;
}

void lapic_eoi(void) {
    if (lapic_virtual_base == 0) return;
    volatile uint32_t *eoi_reg = (volatile uint32_t*)(lapic_virtual_base + LAPIC_EOI_REG);
    *eoi_reg = 0;
}

// Send Inter-Processor Interrupts (IPI) supporting vector and mode flags
void lapic_send_ipi(uint8_t target_apic_id, uint32_t flags) {
    if (lapic_virtual_base == 0) return;

    volatile uint32_t *icr_high = (volatile uint32_t*)(lapic_virtual_base + LAPIC_ICR_HIGH);
    volatile uint32_t *icr_low  = (volatile uint32_t*)(lapic_virtual_base + LAPIC_ICR_LOW);

    // Wait for previous transmission delivery to finish (Bit 12 clear)
    while (*icr_low & (1 << 12)) {
        asm volatile("pause");
    }

    *icr_high = (uint32_t)target_apic_id << 24;
    *icr_low  = flags;
}

void parse_madt(struct acpi_table_madt *madt) {
    if (madt == NULL) return;

    uint64_t lapic_phys_addr = madt->local_apic_address;

    // Initialize standard Identity Overrides
    for (int i = 0; i < MAX_OVERRIDES; i++) {
        isa_overrides[i].irq_source = i;
        isa_overrides[i].gsi = i;
    }

    uintptr_t current_addr = (uintptr_t)madt + sizeof(struct acpi_table_madt);
    uintptr_t end_addr = (uintptr_t)madt + madt->header.length;

    while (current_addr < end_addr) {
        struct madt_record_header *record = (struct madt_record_header *)current_addr;

        if (record->length == 0 || (current_addr + record->length) > end_addr) {
            printk(LOG_ERROR, "MADT parsing error: invalid record length %d\n", record->length);
            break;
        }

        switch (record->type) {
            case 0: { // Local APIC
                struct madt_local_apic *lapic = (struct madt_local_apic *)record;
                // Bit 0 = Processor Enabled, Bit 1 = Online Capable
                if ((lapic->flags & 1) || (lapic->flags & 2)) {
                    if (cpu_count < MAX_CPUS) {
                        cpus[cpu_count++] = *lapic;
                    }
                }
                break;
            }
            case 1: { // I/O APIC
                struct madt_io_apic *ioapic = (struct madt_io_apic *)record;
                ioapic_init(ioapic->io_apic_address);
                break;
            }
            case 2: { // Interrupt Source Override
                struct madt_interrupt_override *override = (struct madt_interrupt_override *)record;
                if (override->irq_source < MAX_OVERRIDES) {
                    isa_overrides[override->irq_source].gsi = override->gsi;
                    isa_overrides[override->irq_source].flags = override->flags;
                }
                break;
            }
            case 5: { // 64-Bit LAPIC Address Override
                struct madt_local_apic_override *lapic_64 = (struct madt_local_apic_override *)record;
                lapic_phys_addr = lapic_64->local_apic_address_64;
                break;
            }
            default:
                break;
        }

        current_addr += record->length;
    }

    // Set base for virtual accessing
    lapic_set_base(lapic_phys_addr);
    printk(LOG_TRACE, "Parsed MADT: Found %d active CPUs. LAPIC Base: 0x%llx\n", cpu_count, lapic_phys_addr);
}

void pic_disable(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

// Dynamic Hardware IRQ routing helper to direct peripheral interrupts to any target CPU core
void ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t target_apic_id) {
    uint32_t gsi = (irq < MAX_OVERRIDES) ? isa_overrides[irq].gsi : irq;
    ioapic_set_entry(ioapic_virtual_base, gsi, vector, target_apic_id);
}

void ioapic(struct acpi_table_madt* madt) {
    // 1. Fully disable legacy 8259 PIC
    pic_disable();

    // 2. Discover CPUs, LAPICs, and IOAPICs
    parse_madt(madt);

    // 3. Initialize the BSP (Bootstrap Processor) Local APIC
    lapic_enable();
    bsp_apic_id = lapic_get_id();

    // 4. Default IOAPIC Interrupt Routes targeting BSP core
    ioapic_route_irq(0,  0x20, bsp_apic_id); // System Timer (PIT/HPET) -> Vector 32
    ioapic_route_irq(1,  0x21, bsp_apic_id); // PS/2 Keyboard          -> Vector 33
    ioapic_route_irq(12, 0x2C, bsp_apic_id); // PS/2 Mouse             -> Vector 44
}