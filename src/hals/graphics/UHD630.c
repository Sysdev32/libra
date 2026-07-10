#include <hals/pci.h>
#include <drivers/fb.h>
#include <drivers/alloc.h>
#include <hals/ahci.h>

// Assuming LOG_TRACE, LOG_WARNING, LOG_ERROR, and HHDM_OFFSET are already defined globally in headers

extern pci_device_t* devices;
extern uint32_t devicecount;

// Internal helper to align raw kmalloc to 4KB pages
void* kmalloc_aligned(size_t size, size_t alignment) {
    size_t total_size = size + alignment + sizeof(void*);
    void* raw_mem = kmalloc(total_size);
    if (!raw_mem) return NULL;

    uintptr_t raw_addr = (uintptr_t)raw_mem;
    uintptr_t aligned_addr = (raw_addr + alignment + sizeof(void*)) & ~(alignment - 1);
    ((void**)aligned_addr)[-1] = raw_mem;

    return (void*)aligned_addr;
}

void init_uhd630() {
    pci_device_t device;
    int found = 0;

    printk(LOG_TRACE, "Intel UHD 630 Initialization started.\n");
    printk(LOG_TRACE, "Scanning PCI bus for compatible Display Controllers...\n");

    for (int i = 0; i < devicecount; i++) {
        int d = devices[i].device_id;
        if (devices[i].class_code == 3 && devices[i].subclass == 0 && 
           (d == 0x3E9B || d == 0x9BC5 || d == 0x3E92 || d == 0x3E98 || d == 0x3E91 || d == 0x9BC4)) {
            device = devices[i];
            found = 1;
            printk(LOG_TRACE, "Target GPU Found! Class: %d, Subclass: %d, Device ID: 0x%X\n", 
                   devices[i].class_code, devices[i].subclass, devices[i].device_id);
            break; 
        }
    }

    if (!found) {
        printk(LOG_ERROR, "Failed to locate an Intel UHD 630 compatible display controller on PCI bus.\n");
        return;
    }

    // ============================================================================
    // BAR 4: GTT APERTURE MAPPING
    // ============================================================================
    printk(LOG_TRACE, "Reading PCI Configuration Space Offset 0x20 (BAR 4)...\n");
    uint32_t bar4_val = pci_read32(device.bus, device.device, device.function, 0x20);
    uint64_t bar4_phys = bar4_val & 0xFFFFF000;
    uint64_t bar4_virtual = bar4_phys + HHDM_OFFSET;

    printk(LOG_TRACE, "BAR 4 Physical Base Address isolated at: 0x%lX\n", bar4_phys);
    printk(LOG_TRACE, "BAR 4 Virtual Destination Target set at: 0x%lX\n", bar4_virtual);

    page_table_t *pml4 = vmm_get_current_pml4();
    printk(LOG_TRACE, "Current address space PML4 retrieved successfully.\n");

    printk(LOG_TRACE, "Mapping 8MB GTT space page-by-page into kernel virtual address space...\n");
    for (uint64_t offset = 0; offset < (8 * 1024 * 1024); offset += 0x1000) {
        vmm_map_page(pml4, bar4_virtual + offset, bar4_phys + offset, PTE_WRITABLE);
    }
    printk(LOG_TRACE, "BAR 4 GTT Aperture mapping phase finalized.\n");

    // ============================================================================
    // BAR 0: MMIO CONTROL REGISTERS MAPPING
    // ============================================================================
    printk(LOG_TRACE, "Reading PCI Configuration Space Offset 0x10 (BAR 0)...\n");
    uint32_t bar0_val = pci_read32(device.bus, device.device, device.function, 0x10);
    uint64_t bar0_phys = bar0_val & 0xFFFFF000; 
    uint64_t bar0_virtual = bar0_phys + HHDM_OFFSET;

    printk(LOG_TRACE, "BAR 0 Physical Base Address isolated at: 0x%lX\n", bar0_phys);
    printk(LOG_TRACE, "BAR 0 Virtual Destination Target set at: 0x%lX\n", bar0_virtual);

    printk(LOG_TRACE, "Mapping 16MB MMIO Space page-by-page into kernel virtual address space...\n");
    for (uint64_t offset = 0; offset < (16 * 1024 * 1024); offset += 0x1000) {
        vmm_map_page(pml4, bar0_virtual + offset, bar0_phys + offset, PTE_WRITABLE);
    }
    printk(LOG_TRACE, "BAR 0 MMIO Control Register space mapping finalized.\n");

    // Establish access pointers
    volatile uint64_t* gtt = (volatile uint64_t*)bar4_virtual;

    // ============================================================================
    // STEP 1: ALLOCATE, ZERO OUT, AND MAP THE FRAMEBUFFER (8 MB)
    // ============================================================================
    printk(LOG_TRACE, "Allocating 8MB page-aligned memory backing block for active frame allocation...\n");
    uint32_t* virtual_framebuffer = (uint32_t*)kmalloc_aligned(8 * 1024 * 1024, 4096); 
    if (!virtual_framebuffer) {
        printk(LOG_ERROR, "Memory allocation routine failed to provision 8MB backing block!\n");
        return;
    }
    uint64_t fb_base_physical = (uint64_t)virtual_framebuffer - HHDM_OFFSET;
    printk(LOG_TRACE, "Framebuffer allocated at V: 0x%lX (P: 0x%lX)\n", (uint64_t)virtual_framebuffer, fb_base_physical);

    printk(LOG_TRACE, "Clearing Framebuffer buffer data bytes explicitly to 0x00000000 (Black color states)...\n");
    for (uint32_t pixel_idx = 0; pixel_idx < (8 * 1024 * 1024 / 4); pixel_idx++) {
        virtual_framebuffer[pixel_idx] = 0x00000000; 
    }

    printk(LOG_TRACE, "Writing physical mapping references sequentially into global GTT structural indices...\n");
    for (uint64_t i = 0; i < 2048; i++) {
        uint64_t phys_page = fb_base_physical + (i * 0x1000);
        gtt[i] = (phys_page & 0xFFFFF000) | 0x3; 
    }
    printk(LOG_TRACE, "GTT Mapping updates successfully committed for Framebuffer range.\n");

    // ============================================================================
    // STEP 2: ALLOCATE AND MAP THE RING BUFFER (16 KB)
    // ============================================================================
    printk(LOG_TRACE, "Allocating 16KB page-aligned command loop Ring Buffer tracking structures...\n");
    uint32_t* virtual_ringbuffer = (uint32_t*)kmalloc_aligned(16 * 1024, 4096);
    if (!virtual_ringbuffer) {
        printk(LOG_ERROR, "Memory allocation routine failed to provision 16KB hardware ring buffer!\n");
        return;
    }
    uint64_t ring_base_physical = (uint64_t)virtual_ringbuffer - HHDM_OFFSET;
    printk(LOG_TRACE, "Ring Buffer allocated at V: 0x%lX (P: 0x%lX)\n", (uint64_t)virtual_ringbuffer, ring_base_physical);

    printk(LOG_TRACE, "Writing Ring Buffer entries sequentially into GTT mapping index offsets starting at index 2048...\n");
    for (uint64_t i = 0; i < 4; i++) {
        uint64_t phys_page = ring_base_physical + (i * 0x1000);
        gtt[2048 + i] = (phys_page & 0xFFFFF000) | 0x3;
    }
    printk(LOG_TRACE, "GTT Mapping updates successfully committed for Ring Buffer space.\n");

    // ============================================================================
    // STEP 3: FLUSH THE IN-GPU GTT CACHE (TLB)
    // ============================================================================
    printk(LOG_TRACE, "Sending Cache invalidation signal down MMIO pipeline to flush GTT cache line addresses...\n");
    volatile uint32_t* gtt_flush = (volatile uint32_t*)((uintptr_t)bar0_virtual + 0x1010);

    *gtt_flush = 0x1; 

    printk(LOG_TRACE, "Polled loop execution checking status register response for hardware cache flush completion...\n");
    while (*gtt_flush & 0x1) {
        __builtin_ia32_pause(); 
    }
    printk(LOG_TRACE, "GTT Translation Cache structure verified clear by hardware validation.\n");

    // ============================================================================
    // STEP 4: INITIALIZE THE BLITTER ENGINE COMMAND STREAMER REGISTERS
    // ============================================================================
    printk(LOG_TRACE, "Configuring BCS Blitter Engine Command Streamer base parameters...\n");
    
    uint32_t bcs_base = 0x22000;
    volatile uint32_t* bcs_ring_start = (volatile uint32_t*)(bar0_virtual + bcs_base + 0x38);
    volatile uint32_t* bcs_ring_len   = (volatile uint32_t*)(bar0_virtual + bcs_base + 0x3C);
    volatile uint32_t* bcs_ring_head  = (volatile uint32_t*)(bar0_virtual + bcs_base + 0x30);
    volatile uint32_t* bcs_ring_tail  = (volatile uint32_t*)(bar0_virtual + bcs_base + 0x34);

    // Set engine to access Ring Buffer via GPU space mapped offset (GTT entry 2048 * 4096 = 0x00800000)
    *bcs_ring_start = 0x00800000;
    
    // Config length details: Size parameter set based on 4-page scope (0x3000) along with status bit 0 enable flag
    *bcs_ring_len   = 0x3000 | 1;
    *bcs_ring_head  = 0;
    *bcs_ring_tail  = 0;

    printk(LOG_TRACE, "Blitter Command Streamer successfully armed and listening.\n");

    // ============================================================================
    // STEP 5: EMIT 2D BLITTER COMMAND TO DRAW HARDWARE RECTANGLE
    // ============================================================================
    printk(LOG_TRACE, "Preparing drawing command vector structural data...\n");
    
    uint32_t stride = 7680; // 1920 pixels * 4 bytes per pixel
    uint32_t ring_offset = 0;

    // Local function write macros to keep emission linear
    #define EMIT_DWORD(val) do { \
        virtual_ringbuffer[ring_offset / 4] = (val); \
        ring_offset += 4; \
    } while(0)

    // Constructing a solid rectangle box vector command block
    uint16_t x1 = 200, y1 = 200;
    uint16_t x2 = 800, y2 = 600;
    uint32_t rect_color = 0xFF00FF00; // Bright green pixel configuration field color code state

    printk(LOG_TRACE, "Assembling XY_COLOR_BLT hardware token streaming payload data vectors...\n");
    EMIT_DWORD(0x50400004);                  // DWORD 0: Opcode 0x50 | 32-bit color config | size metric count
    EMIT_DWORD(0x00CC0000 | stride);          // DWORD 1: Solid raster configuration payload action | row length target info
    EMIT_DWORD(((uint32_t)y1 << 16) | x1);    // DWORD 2: Y1 upper 16 | X1 lower 16
    EMIT_DWORD(((uint32_t)y2 << 16) | x2);    // DWORD 3: Y2 upper 16 | X2 lower 16
    EMIT_DWORD(0x00000000);                  // DWORD 4: Base location space reference pointing to GTT 0x0 Display mapping
    EMIT_DWORD(rect_color);                  // DWORD 5: Pixel fill values color code data bytes

    printk(LOG_TRACE, "Invalidating cache line bounds for written command packet vectors across system RAM channels...\n");
    
    // Explicit assembly injection replaces the missing compiler intrinsic
    for (uint32_t i = 0; i < 24; i += 64) {
        volatile uint8_t* target_addr = ((volatile uint8_t*)virtual_ringbuffer) + i;
        __asm__ volatile("clflush %0" : "+m" (*target_addr));
    }

    printk(LOG_TRACE, "Committing ring offset tail registry update index position to: %d\n", ring_offset);
    *bcs_ring_tail = ring_offset;

    printk(LOG_TRACE, "Execution instructions dropped. System execution tracking pipeline operating back inside kernel space loop controls.\n");
    #undef EMIT_DWORD
}