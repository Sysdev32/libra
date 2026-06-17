#include <stdint.h>
#include <drivers/idt.h>
#include <drivers/pci.h>
#include <drivers/alloc.h>
#include <drivers/fb.h>
uintptr_t ecamaddr;
uintptr_t calculate_ecam_address(uintptr_t base_address, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset)
{
    uintptr_t address = base_address;

    address |= ((uintptr_t)bus) << 20;      // Bus shifts left by 20 bits
    address |= ((uintptr_t)device) << 15;   // Device shifts left by 15 bits
    address |= ((uintptr_t)function) << 12; // Function shifts left by 12 bits
    address |= offset;                      // Add the register offset

    return address;
}

void parse_mcfg(const void *raw_mcfg_ptr, const MCFG_ALLOCATION **out_allocs, int *out_count)
{
    if (!raw_mcfg_ptr || !out_allocs || !out_count)
    {
        return;
    }

    const MCFG_TABLE *mcfg = (const MCFG_TABLE *)raw_mcfg_ptr;

    // Direct pointer assignment to the start of the flexible array
    *out_allocs = mcfg->Allocations;
    
    // Freestanding size math without offsetof header dependencies
    // Header (36 bytes) + Reserved (8 bytes) = 44 bytes total table overhead
    uint32_t total_length = mcfg->Header.Length;
    uint32_t payload_bytes = total_length - 44;

    // Shift right by 4 instead of dividing by 16 (sizeof(MCFG_ALLOCATION) is 16)
    *out_count = (int)(payload_bytes >> 4);
}
void init_pci(MCFG_TABLE *mcfg)
{
    MCFG_ALLOCATION *allocations;
    int allocation_count = 0;
    allocations = kcalloc(sizeof(MCFG_ALLOCATION), 128);
    parse_mcfg((const void*)mcfg, &allocations, &allocation_count);
}