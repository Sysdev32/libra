#include <stdint.h>

#pragma pack(push, 1)

// Standard ACPI Header (36 bytes)
typedef struct {
    char     Signature[4];
    uint32_t Length;
    uint8_t  Revision;
    uint8_t  Checksum;
    char     OemId[6];
    char     OemTableId[8];
    uint32_t OemRevision;
    uint32_t CreatorId[4];
    uint32_t CreatorRevision;
} ACPI_HEADER;

// MCFG Allocation Record (16 bytes)
typedef struct {
    uint64_t BaseAddress;
    uint16_t PciSegmentGroup;
    uint8_t  StartBusNumber;
    uint8_t  EndBusNumber;
    uint32_t Reserved;
} MCFG_ALLOCATION;

/**
 * Unified MCFG Table Structure
 * Includes a flat array that mirrors hardware memory layout exactly.
 */
typedef struct {
    ACPI_HEADER     Header;          // 36 bytes
    uint64_t        Reserved;        // 8 bytes
    
    // Flexible array member: maps directly to trailing memory bytes
    MCFG_ALLOCATION Allocations[];   
} MCFG_TABLE;

#pragma pack(pop)
void init_pci(MCFG_TABLE *mcfg);