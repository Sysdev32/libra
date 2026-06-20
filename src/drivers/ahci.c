#include <drivers/pci.h>
#include <drivers/alloc.h>
#include <stdint.h>
#include <string.h>
#include <drivers/fb.h>
#include <drivers/ahci.h>
#define PTE_WRITABLE (1ULL << 1)
#define HHDM_OFFSET  0xffff800000000000ULL
#define PAGE_SIZE 4096

// Signature constants defined by the AHCI/SATA specifications
#define AHCI_SIG_SATA   0x00000101  // Standard SATA Hard Drive/SSD
#define AHCI_SIG_ATAPI  0xEB140101  // ATAPI Optical Drive (CD/DVD)

typedef uint64_t page_table_t;
extern page_table_t *vmm_get_current_pml4(void);
extern void vmm_map_page(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
extern void* pmm_alloc_pages(int order); 



// Global instance to hold our main drive pointer
static ahci_device_t primary_sata_drive;

/**
 * Issues an ATA command to an AHCI port.
 */
int ahci_send_command(uint64_t port_base, void* cl_virt, void* table_virt, uint64_t table_phys, 
                      uint8_t ata_command, uint64_t buffer_phys, uint32_t buffer_size, int is_write) {
    
    // Ensure the Command Engine is stopped before updating headers
    *(volatile uint32_t*)(port_base + 0x18) &= ~(1 << 0); // Clear ST (Start)
    
    uint32_t timeout = 1000000;
    while(*(volatile uint32_t*)(port_base + 0x18) & (1 << 15)) { // Wait for CR (Command Running)
        if (--timeout == 0) {
            printk(LOG_DEBUG, "[AHCI CMD ERROR] Engine hang! CR bit failed to clear.\n");
            return 0;
        }
    }

    // Set up Command Header Slot 0
    ahci_cmd_header_t* cmd_hdr = (ahci_cmd_header_t*)cl_virt;
    memset(cmd_hdr, 0, sizeof(ahci_cmd_header_t));
    cmd_hdr->cfl = 5;          
    cmd_hdr->w = is_write ? 1 : 0; 
    cmd_hdr->prdtl = 1;        
    cmd_hdr->ctba = (uint32_t)table_phys;
    cmd_hdr->ctbau = (uint32_t)(table_phys >> 32);

    // Populate Command Table FIS layout
    uint8_t* cmd_table = (uint8_t*)table_virt;
    memset(cmd_table, 0, PAGE_SIZE); 
    
    cmd_table[0] = 0x27;       // FIS Type: Register H2D
    cmd_table[1] = 0x80;       // Main command execution flag bit
    cmd_table[2] = ata_command;
    cmd_table[7] = 0xA0;       // Device register selection

    // Populate PRDT Entry
    ahci_prdt_entry_t* prdt = (ahci_prdt_entry_t*)(cmd_table + 0x80);
    prdt->dba = (uint32_t)buffer_phys;
    prdt->dbau = (uint32_t)(buffer_phys >> 32);
    prdt->dbc = buffer_size - 1; 
    prdt->i = 1;               

    // Fire command processing via slot 0
    *(volatile uint32_t*)(port_base + 0x18) |= (1 << 0); // Set ST (Start)
    *(volatile uint32_t*)(port_base + 0x38) = (1 << 0);  // Set CI (Command Issue) Slot 0

    // Monitor processing state loops
    timeout = 5000000;
    while (1) {
        if ((*(volatile uint32_t*)(port_base + 0x38) & (1 << 0)) == 0) {
            break; // Finished successfully
        }
        
        uint32_t tfd = *(volatile uint32_t*)(port_base + 0x20);
        if (tfd & (1 << 0)) { // Check device error bit
            return 0;
        }

        if (--timeout == 0) {
            return 0;
        }
    }

    return 1;
}

extern pci_device_t* devices;
extern uint32_t devicecount;

void init_ahci() {
    printk(LOG_DEBUG, "[AHCI] Starting controller initialization.\n");
    memset(&primary_sata_drive, 0, sizeof(ahci_device_t));
    
    pci_device_t device;
    memset(&device, 0, sizeof(pci_device_t));
    int found = 0;

    for (int i = 0; i < devicecount; i++) {
        if (devices[i].class_code == 1 && devices[i].subclass == 6) {
            device = devices[i];
            found = 1;
            break; 
        }
    }

    if (!found) {
        printk(LOG_DEBUG, "[AHCI ERROR] No AHCI storage controller found on PCI bus.\n");
        return;
    }

    uint32_t bar5_val = pci_read32(device.bus, device.device, device.function, 0x24);
    uint64_t abar_physical = 0;

    if ((bar5_val & 0x6) == 0x4) {
        uint32_t bar6_val = pci_read32(device.bus, device.device, device.function, 0x28);
        abar_physical = ((uint64_t)bar6_val << 32) | (bar5_val & 0xFFFFFFF0);
    } else {
        abar_physical = bar5_val & 0xFFFFFFF0;
    }

    page_table_t *pml4 = vmm_get_current_pml4();
    uint64_t abar_virtual = abar_physical + HHDM_OFFSET;
    vmm_map_page(pml4, abar_virtual, abar_physical, PTE_WRITABLE);
    
    volatile uint32_t pi = *(volatile uint32_t *)(abar_virtual + 0x0C); 

    for (int i = 0; i < 32; i++) {
        if (pi & (1 << i)) {
            uint64_t port_base = abar_virtual + 0x100 + (i * 0x80);
            
            volatile uint32_t ssts = *(volatile uint32_t *)(port_base + 0x28); 
            volatile uint32_t sig = *(volatile uint32_t *)(port_base + 0x24);
            uint8_t det = ssts & 0xF;
            
            if (det == 3) { 
                // --- ATAPI IGNORE FILTER ---
                if (sig == AHCI_SIG_ATAPI) {
                    printk(LOG_DEBUG, "[AHCI PORT %d] CD-ROM/ATAPI device ignored.\n", i);
                    continue; 
                }

                if (sig != AHCI_SIG_SATA) {
                    printk(LOG_DEBUG, "[AHCI PORT %d] Unknown device signature (0x%x) ignored.\n", i, sig);
                    continue;
                }

                printk(LOG_DEBUG, "[AHCI PORT %d] SATA Hard Drive detected. Initializing storage handles...\n", i);
                
                void* cl_virt = pmm_alloc_pages(0); 
                uint64_t cl_phys = (uint64_t)cl_virt - HHDM_OFFSET;

                void* fis_virt = pmm_alloc_pages(0);
                uint64_t fis_phys = (uint64_t)fis_virt - HHDM_OFFSET;

                void* table_virt = pmm_alloc_pages(0);
                uint64_t table_phys = (uint64_t)table_virt - HHDM_OFFSET;

                void* identify_buf_virt = pmm_alloc_pages(0); 
                uint64_t identify_buf_phys = (uint64_t)identify_buf_virt - HHDM_OFFSET;

                memset(cl_virt, 0, PAGE_SIZE);
                memset(fis_virt, 0, PAGE_SIZE);
                memset(table_virt, 0, PAGE_SIZE);
                memset(identify_buf_virt, 0, PAGE_SIZE);

                // Safely bring down port engines to set up DMA registers
                *(volatile uint32_t*)(port_base + 0x18) &= ~(1 << 0); // Clear ST
                *(volatile uint32_t*)(port_base + 0x18) &= ~(1 << 4); // Clear FRE

                while((*(volatile uint32_t*)(port_base + 0x18) & (1 << 15)) || 
                      (*(volatile uint32_t*)(port_base + 0x18) & (1 << 14)));

                *(volatile uint32_t*)(port_base + 0x00) = (uint32_t)cl_phys;         
                *(volatile uint32_t*)(port_base + 0x04) = (uint32_t)(cl_phys >> 32);  
                *(volatile uint32_t*)(port_base + 0x08) = (uint32_t)fis_phys;         
                *(volatile uint32_t*)(port_base + 0x0C) = (uint32_t)(fis_phys >> 32);  

                *(volatile uint32_t*)(port_base + 0x18) |= (1 << 4); // Restart FRE

                int cmd_success = ahci_send_command(port_base, cl_virt, table_virt, table_phys, 
                                                    0xEC, identify_buf_phys, 512, 0);

                if (!cmd_success) {
                    printk(LOG_DEBUG, "[AHCI PORT %d] IDENTIFY DEVICE failed.\n", i);
                    continue;
                }

                uint16_t* identify_data = (uint16_t*)identify_buf_virt;
                uint64_t total_sectors = 0;

                if (identify_data[83] & (1 << 10)) {
                    total_sectors = ((uint64_t)identify_data[103] << 48) |
                                    ((uint64_t)identify_data[102] << 32) |
                                    ((uint64_t)identify_data[101] << 16) |
                                     (uint64_t)identify_data[100];
                } else {
                    total_sectors = ((uint32_t)identify_data[61] << 16) | 
                                     (uint32_t)identify_data[60];
                }

                uint64_t drive_gb = (total_sectors * 512) / (1024 * 1024 * 1024);
                
                // --- STORE DRIVE POINTER INFO ---
                primary_sata_drive.port_base = port_base;
                primary_sata_drive.cl_virt = cl_virt;
                primary_sata_drive.table_virt = table_virt;
                primary_sata_drive.table_phys = table_phys;
                primary_sata_drive.total_sectors = total_sectors;
                primary_sata_drive.size_gb = (uint32_t)drive_gb;
                primary_sata_drive.port_number = i;
                primary_sata_drive.is_initialized = 1;

                printk(LOG_DEBUG, "[AHCI] Registered main drive on Port %d (%d GB).\n", i, primary_sata_drive.size_gb);
                break; // Stop scanning once we've safely initialized our primary hard drive pointer
            }
        }
    }
}

/**
 * External getter function allowing other parts of your kernel code 
 * (like VFS or file system partition mounters) to access the drive data.
 */
ahci_device_t* get_primary_sata_drive(void) {
    if (primary_sata_drive.is_initialized) {
        return &primary_sata_drive;
    }
    return NULL; // Returns null if no valid SATA hard drive is initialized
}
/**
 * Reads sectors from the primary SATA drive using 48-bit LBA DMA.
 * @param drive      Pointer to your initialized ahci_device_t
 * @param start_lba  The logical block address to start reading from
 * @param count      Number of sectors to read (1 sector = 512 bytes)
 * @param buf_phys   PHYSICAL memory address where the disk data will be copied
 */
int ahci_read_sectors(ahci_device_t* drive, uint64_t start_lba, uint16_t count, uint64_t buf_phys) {
    if (!drive || !drive->is_initialized) {
        printk(LOG_DEBUG, "[AHCI READ ERROR] Attempted to read from an uninitialized drive.\n");
        return 0;
    }

    uint64_t port_base = drive->port_base;

    // 1. Stop the command engine before preparing slot 0
    *(volatile uint32_t*)(port_base + 0x18) &= ~(1 << 0); // Clear ST
    uint32_t timeout = 1000000;
    while(*(volatile uint32_t*)(port_base + 0x18) & (1 << 15)) {
        if (--timeout == 0) return 0;
    }

    // 2. Set up Command Header Slot 0 (Read operation, w = 0)
    ahci_cmd_header_t* cmd_hdr = (ahci_cmd_header_t*)drive->cl_virt;
    memset(cmd_hdr, 0, sizeof(ahci_cmd_header_t));
    cmd_hdr->cfl = 5;          // FIS length: 5 dwords
    cmd_hdr->w = 0;            // 0 = Read from disk to memory
    cmd_hdr->prdtl = 1;        // Using 1 PRDT entry
    cmd_hdr->ctba = (uint32_t)drive->table_phys;
    cmd_hdr->ctbau = (uint32_t)(drive->table_phys >> 32);

    // 3. Set up Command Table FIS (Register H2D layout for READ DMA EXT)
    uint8_t* cmd_table = (uint8_t*)drive->table_virt;
    memset(cmd_table, 0, PAGE_SIZE);
    
    cmd_table[0] = 0x27;       // FIS Type: Register H2D
    cmd_table[1] = 0x80;       // Command bit set
    cmd_table[2] = 0x25;       // ATA Command: READ DMA EXT (0x25)
    cmd_table[7] = 0x40;       // LBA mode bit flag set

    // Pack the 48-bit LBA address into the FIS layout
    cmd_table[4] = (uint8_t)start_lba;          // LBA low (0:7)
    cmd_table[5] = (uint8_t)(start_lba >> 8);   // LBA mid (8:15)
    cmd_table[6] = (uint8_t)(start_lba >> 16);  // LBA high (16:23)
    cmd_table[8] = (uint8_t)(start_lba >> 24);  // LBA 4 (24:31)
    cmd_table[9] = (uint8_t)(start_lba >> 32);  // LBA 5 (32:39)
    cmd_table[10] = (uint8_t)(start_lba >> 40); // LBA 6 (40:47)

    // Pack the sector count (16-bit)
    cmd_table[12] = (uint8_t)count;             // Count low (0:7)
    cmd_table[13] = (uint8_t)(count >> 8);      // Count high (8:15)

    // 4. Point the PRDT entry directly to your destination buffer
    ahci_prdt_entry_t* prdt = (ahci_prdt_entry_t*)(cmd_table + 0x80);
    prdt->dba = (uint32_t)buf_phys;
    prdt->dbau = (uint32_t)(buf_phys >> 32);
    
    // Total transfer byte count calculation (sectors * 512) - 1
    // Bit 0 must be 1 to signal an interrupt on completion to the host
    prdt->dbc = ((uint32_t)count * 512) - 1;
    prdt->i = 1;

    // 5. Fire the engine and issue the command
    *(volatile uint32_t*)(port_base + 0x18) |= (1 << 0); // Set ST
    *(volatile uint32_t*)(port_base + 0x38) = (1 << 0);  // Set CI Slot 0

    // 6. Wait for the controller to finish reading
    timeout = 5000000;
    while (1) {
        if ((*(volatile uint32_t*)(port_base + 0x38) & (1 << 0)) == 0) {
            break; // Success! The controller cleared the bit
        }
        uint32_t tfd = *(volatile uint32_t*)(port_base + 0x20);
        if (tfd & (1 << 0)) { // Error bit check
            printk(LOG_DEBUG, "[AHCI READ ERROR] Hardware error during read! TFD: 0x%x\n", tfd);
            return 0;
        }
        if (--timeout == 0) {
            printk(LOG_DEBUG, "[AHCI READ ERROR] Read transaction timed out!\n");
            return 0;
        }
    }

    return 1;
}
/**
 * Writes sectors to the primary SATA drive using 48-bit LBA DMA.
 * @param drive      Pointer to your initialized ahci_device_t
 * @param start_lba  The logical block address to start writing to
 * @param count      Number of sectors to write (1 sector = 512 bytes)
 * @param buf_phys   PHYSICAL memory address where the data to write is stored
 */
int ahci_write_sectors(ahci_device_t* drive, uint64_t start_lba, uint16_t count, uint64_t buf_phys) {
    if (!drive || !drive->is_initialized) {
        printk(LOG_DEBUG, "[AHCI WRITE ERROR] Attempted to write to an uninitialized drive.\n");
        return 0;
    }

    uint64_t port_base = drive->port_base;

    // 1. Stop the command engine before preparing slot 0
    *(volatile uint32_t*)(port_base + 0x18) &= ~(1 << 0); // Clear ST
    uint32_t timeout = 1000000;
    while(*(volatile uint32_t*)(port_base + 0x18) & (1 << 15)) {
        if (--timeout == 0) return 0;
    }

    // 2. Set up Command Header Slot 0 (Write operation, w = 1)
    ahci_cmd_header_t* cmd_hdr = (ahci_cmd_header_t*)drive->cl_virt;
    memset(cmd_hdr, 0, sizeof(ahci_cmd_header_t));
    cmd_hdr->cfl = 5;          // FIS length: 5 dwords
    cmd_hdr->w = 1;            // 1 = Write from memory to disk
    cmd_hdr->prdtl = 1;        // Using 1 PRDT entry
    cmd_hdr->ctba = (uint32_t)drive->table_phys;
    cmd_hdr->ctbau = (uint32_t)(drive->table_phys >> 32);

    // 3. Set up Command Table FIS (Register H2D layout for WRITE DMA EXT)
    uint8_t* cmd_table = (uint8_t*)drive->table_virt;
    memset(cmd_table, 0, PAGE_SIZE);
    
    cmd_table[0] = 0x27;       // FIS Type: Register H2D
    cmd_table[1] = 0x80;       // Command bit set
    cmd_table[2] = 0x35;       // ATA Command: WRITE DMA EXT (0x35)
    cmd_table[7] = 0x40;       // LBA mode bit flag set

    // Pack the 48-bit LBA address into the FIS layout
    cmd_table[4] = (uint8_t)start_lba;          
    cmd_table[5] = (uint8_t)(start_lba >> 8);   
    cmd_table[6] = (uint8_t)(start_lba >> 16);  
    cmd_table[8] = (uint8_t)(start_lba >> 24);  
    cmd_table[9] = (uint8_t)(start_lba >> 32);  
    cmd_table[10] = (uint8_t)(start_lba >> 40); 

    // Pack the sector count (16-bit)
    cmd_table[12] = (uint8_t)count;             
    cmd_table[13] = (uint8_t)(count >> 8);      

    // 4. Point the PRDT entry to your source data buffer
    ahci_prdt_entry_t* prdt = (ahci_prdt_entry_t*)(cmd_table + 0x80);
    prdt->dba = (uint32_t)buf_phys;
    prdt->dbau = (uint32_t)(buf_phys >> 32);
    
    // Total transfer byte count calculation (sectors * 512) - 1
    prdt->dbc = ((uint32_t)count * 512) - 1;
    prdt->i = 1; // Interrupt on completion

    // 5. Fire the engine and issue the command
    *(volatile uint32_t*)(port_base + 0x18) |= (1 << 0); // Set ST
    *(volatile uint32_t*)(port_base + 0x38) = (1 << 0);  // Set CI Slot 0

    // 6. Monitor processing state loops
    timeout = 5000000;
    while (1) {
        if ((*(volatile uint32_t*)(port_base + 0x38) & (1 << 0)) == 0) {
            break; // Success! The controller cleared the bit
        }
        uint32_t tfd = *(volatile uint32_t*)(port_base + 0x20);
        if (tfd & (1 << 0)) { // Error bit check
            printk(LOG_DEBUG, "[AHCI WRITE ERROR] Hardware error during write! TFD: 0x%x\n", tfd);
            return 0;
        }
        if (--timeout == 0) {
            printk(LOG_DEBUG, "[AHCI WRITE ERROR] Write transaction timed out!\n");
            return 0;
        }
    }

    return 1;
}