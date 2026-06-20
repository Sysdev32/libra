#pragma once
#include <stdint.h>
// Structure used to pass handle pointers to other kernel components
typedef struct {
    uint64_t port_base;         // MMIO Address of this specific port
    void* cl_virt;              // Virtual pointer to Command List
    void* table_virt;           // Virtual pointer to Command Table
    uint64_t table_phys;        // Physical address of Command Table
    uint64_t total_sectors;     // Capacity of the drive in 512-byte sectors
    uint32_t size_gb;           // Capacity in Gigabytes
    uint8_t port_number;        // Hardware index number (0-31)
    uint8_t is_initialized;     // Flag confirming driver handshake state
} ahci_device_t;

// AHCI Command Header structure
typedef struct {
    uint8_t  cfl:5;     // Command FIS length in dwords
    uint8_t  a:1;       // ATAPI flag (0 = SATA, 1 = ATAPI)
    uint8_t  w:1;       // Write flag (0 = Read, 1 = Write)
    uint8_t  p:1;       // Prefetchable
    uint8_t  r:1;       // Reset
    uint8_t  b:1;       // BIST
    uint8_t  c:1;       // Clear busy upon R_OK
    uint8_t  reserved0:1;
    uint8_t  pmp:4;     // Port multiplier port
    uint16_t prdtl;     // Physical Region Descriptor Table Length
    volatile uint32_t prdbc; // Physical Region Descriptor Byte Count
    uint32_t ctba;      // Command table descriptor base address
    uint32_t ctbau;     // Command table descriptor base address upper
    uint32_t reserved1[4];
} __attribute__((packed)) ahci_cmd_header_t;

// AHCI PRDT Entry structure
typedef struct {
    uint32_t dba;       // Data base address
    uint32_t dbau;      // Data base address upper
    uint32_t reserved0;
    uint32_t dbc:22;    // Byte count (0-indexed, so size - 1)
    uint32_t reserved1:9;
    uint32_t i:1;       // Interrupt on completion
} __attribute__((packed)) ahci_prdt_entry_t;
int ahci_read_sectors(ahci_device_t* drive, uint64_t start_lba, uint16_t count, uint64_t buf_phys);
int ahci_write_sectors(ahci_device_t* drive, uint64_t start_lba, uint16_t count, uint64_t buf_phys);
void init_ahci();