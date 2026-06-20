#include <stdint.h>
#include <drivers/pci.h>
// Port addresses for PCI Configuration Mechanism 1
#define PCI_CONFIG_ADDRESS 0x0CF8
#define PCI_CONFIG_DATA    0x0CFC
static pci_device_t pci_inventory[MAX_PCI_DEVICES];
static uint32_t pci_inventory_count = 0;

// Filter output buffer for queries
static pci_device_t pci_filter_results[MAX_PCI_DEVICES];

/**
 * Internal helper to select the target device register.
 */
static inline void pci_set_address(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = 0;
    address |= ((uint32_t)1 << 31);                 // Bit 31: Enable bit
    address |= ((uint32_t)bus << 16);               // Bits 23-16: Bus Number
    address |= ((uint32_t)(device & 0x1F) << 11);    // Bits 15-11: Device Number
    address |= ((uint32_t)(function & 0x07) << 8);  // Bits 10-8: Function Number
    address |= ((uint32_t)(offset & 0xFC));         // Bits 7-2: Register Offset

    // Fixed: Forced "d" constraint maps the port to DX explicitly
    uint16_t port = PCI_CONFIG_ADDRESS;
    __asm__ volatile("outl %0, %1" : : "a"(address), "d"(port));
}


/* ==========================================
 *              READ FUNCTIONS
 * ========================================== */

uint32_t pci_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    pci_set_address(bus, device, function, offset);
    uint32_t data;
    uint16_t port = PCI_CONFIG_DATA;
    __asm__ volatile("inl %1, %0" : "=a"(data) : "d"(port));
    return data;
}

uint16_t pci_read16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    pci_set_address(bus, device, function, offset);
    uint16_t data;
    uint16_t port = (uint16_t)(PCI_CONFIG_DATA + (offset & 2));
    __asm__ volatile("inw %1, %0" : "=a"(data) : "d"(port));
    return data;
}

uint8_t pci_read8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    pci_set_address(bus, device, function, offset);
    uint8_t data;
    uint16_t port = (uint16_t)(PCI_CONFIG_DATA + (offset & 3));
    __asm__ volatile("inb %1, %0" : "=a"(data) : "d"(port));
    return data;
}

/* ==========================================
 *              WRITE FUNCTIONS
 * ========================================== */

void pci_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    pci_set_address(bus, device, function, offset);
    uint16_t port = PCI_CONFIG_DATA;
    __asm__ volatile("outl %0, %1" : : "a"(value), "d"(port));
}

void pci_write16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value) {
    pci_set_address(bus, device, function, offset);
    uint16_t port = (uint16_t)(PCI_CONFIG_DATA + (offset & 2));
    __asm__ volatile("outw %0, %1" : : "a"(value), "d"(port));
}

void pci_write8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value) {
    pci_set_address(bus, device, function, offset);
    uint16_t port = (uint16_t)(PCI_CONFIG_DATA + (offset & 3));
    __asm__ volatile("outb %0, %1" : : "a"(value), "d"(port));
}

void pci_scan_bus(pci_device_t* out_buffer, uint32_t max_capacity, uint32_t* out_count) {
    uint32_t count = 0;

    // Guard against NULL parameters
    if (!out_buffer || !out_count || max_capacity == 0) {
        if (out_count) *out_count = 0;
        return;
    }

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            
            // Check Function 0 to see if anything is connected to the slot
            uint16_t vendor_id = pci_read16((uint8_t)bus, dev, 0, 0x00);
            if (vendor_id == 0xFFFF || vendor_id == 0x0000) {
                continue; 
            }

            // Determine if multi-function device (Bit 7 of Header Type register)
            uint8_t header_type = pci_read8((uint8_t)bus, dev, 0, 0x0E);
            uint8_t max_functions = (header_type & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < max_functions; func++) {
                // Prevent out-of-bounds writing to user memory
                if (count >= max_capacity) {
                    *out_count = count;
                    return;
                }

                uint16_t func_vendor = pci_read16((uint8_t)bus, dev, func, 0x00);
                if (func_vendor == 0xFFFF || func_vendor == 0x0000) {
                    continue;
                }

                // Collect configuration metrics directly into the destination buffer
                pci_device_t* dev_slot = &out_buffer[count];
                dev_slot->vendor_id  = func_vendor;
                dev_slot->device_id  = pci_read16((uint8_t)bus, dev, func, 0x02);
                dev_slot->bus        = (uint8_t)bus;
                dev_slot->device     = dev;
                dev_slot->function   = func;
                dev_slot->class_code = pci_read8((uint8_t)bus, dev, func, 0x0B);
                dev_slot->subclass   = pci_read8((uint8_t)bus, dev, func, 0x0A);

                count++;
            }
        }
    }

    *out_count = count;
}

/* ==========================================
 *          FILTER BY CLASS & SUBCLASS
 * ========================================== */

/**
 * Filters a source inventory of devices, writing matches to a separate user-provided output buffer.
 * 
 * @param src_inventory Pointer to the array containing the full list of devices to filter.
 * @param src_count     Total number of entries in the source array.
 * @param class_code    Target hardware category class (e.g., 0x01 for Storage)
 * @param subclass      Target hardware subcategory (e.g., 0x06 for SATA)
 * @param out_buffer    Pointer to a user-allocated array where matches will be written.
 * @param max_capacity  Maximum number of items out_buffer can safely hold.
 * @param out_count     Pointer to a variable where the total matched count will be saved.
 */
void pci_find_by_class(const pci_device_t* src_inventory, uint32_t src_count, 
                       uint8_t class_code, uint8_t subclass, 
                       pci_device_t* out_buffer, uint32_t max_capacity, uint32_t* out_count) {
    uint32_t match_count = 0;

    // Guard against NULL parameters
    if (!src_inventory || !out_buffer || !out_count || max_capacity == 0) {
        if (out_count) *out_count = 0;
        return;
    }

    for (uint32_t i = 0; i < src_count; i++) {
        if (src_inventory[i].class_code == class_code && src_inventory[i].subclass == subclass) {
            
            // Prevent writing beyond user destination buffer size
            if (match_count >= max_capacity) {
                break;
            }

            out_buffer[match_count] = src_inventory[i];
            match_count++;
        }
    }

    *out_count = match_count;
}