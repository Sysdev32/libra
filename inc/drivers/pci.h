#include <stdint.h>
#define MAX_PCI_DEVICES 128

/**
 * Packed structural layout for a discovered PCI device instance.
 */
typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
} pci_device_t;
uint32_t pci_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint16_t pci_read16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint8_t pci_read8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void pci_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);
void pci_write16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value);
void pci_write8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value);
void pci_scan_bus(pci_device_t* out_buffer, uint32_t max_capacity, uint32_t* out_count);
void pci_find_by_class(const pci_device_t* src_inventory, uint32_t src_count, 
                       uint8_t class_code, uint8_t subclass, 
                       pci_device_t* out_buffer, uint32_t max_capacity, uint32_t* out_count);