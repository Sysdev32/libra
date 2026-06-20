#include <drivers/pci.h>
#include <drivers/alloc.h>
#include <stdint.h>
#include <string.h>
#include <drivers/fb.h>
#include <drivers/ahci.h>

typedef uint64_t page_table_t;
extern page_table_t *vmm_get_current_pml4(void);
extern void vmm_map_page(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
extern void* pmm_alloc_pages(int order); 

static ahci_device_t primary_sata_drive;

int ahci_send_command(uint64_t port_base, void* cl_virt, void* table_virt, uint64_t table_phys, 
                      uint8_t ata_command, uint64_t buffer_phys, uint32_t buffer_size, int is_write) {
    
    printk(LOG_DEBUG, "[AHCI CMD] Sending command 0x%x. Buffer Phys: 0x%llx, Size: %u, Write: %d\n", 
           ata_command, buffer_phys, buffer_size, is_write);

    // 1. Ensure the port engine is stopped before modification
    uint32_t pxcmd = *(volatile uint32_t*)(port_base + 0x18);
    *(volatile uint32_t*)(port_base + 0x18) &= ~(1 << 0);
    
    uint32_t timeout = 1000000;
    while(*(volatile uint32_t*)(port_base + 0x18) & (1 << 15)) {
        if (--timeout == 0) {
            printk(LOG_DEBUG, "[AHCI CMD CRITICAL ERROR] Engine hang! CR bit (bit 15) failed to clear.\n");
            return 0;
        }
    }

    // 2. Clear and set up Command Header Slot 0
    ahci_cmd_header_t* cmd_hdr = (ahci_cmd_header_t*)cl_virt;
    memset(cmd_hdr, 0, sizeof(ahci_cmd_header_t));
    cmd_hdr->cfl = 5;          
    cmd_hdr->w = is_write ? 1 : 0; 
    cmd_hdr->prdtl = 1;        
    cmd_hdr->ctba = (uint32_t)table_phys;
    cmd_hdr->ctbau = (uint32_t)(table_phys >> 32);

    // 3. Populate Command Table FIS layout
    uint8_t* cmd_table = (uint8_t*)table_virt;
    memset(cmd_table, 0, PAGE_SIZE); 
    
    cmd_table[0] = 0x27;       // FIS Type: Register H2D
    cmd_table[1] = 0x80;       // Command bit set
    cmd_table[2] = ata_command;
    cmd_table[7] = 0xA0;       

    // 4. Populate PRDT Entry (Splitting 64-bit physical addresses correctly)
    ahci_prdt_entry_t* prdt = (ahci_prdt_entry_t*)(cmd_table + 0x80);
    prdt->dba = (uint32_t)buffer_phys;
    prdt->dbau = (uint32_t)(buffer_phys >> 32);
    prdt->dbc = buffer_size - 1; 
    prdt->i = 1;               
    
    printk(LOG_DEBUG, "[AHCI CMD] PRDT written -> DBA: 0x%x, DBAU: 0x%x, DBC: %u\n", prdt->dba, prdt->dbau, prdt->dbc);

    // 5. Fire command processing via slot 0
    *(volatile uint32_t*)(port_base + 0x18) |= (1 << 0); 
    *(volatile uint32_t*)(port_base + 0x38) = (1 << 0);  

    // 6. Monitor processing state loops
    timeout = 5000000;
    while (1) {
        uint32_t ci = *(volatile uint32_t*)(port_base + 0x38);
        if ((ci & (1 << 0)) == 0) {
            break; 
        }
        
        uint32_t tfd = *(volatile uint32_t*)(port_base + 0x20);
        if (tfd & (1 << 0)) { 
            printk(LOG_DEBUG, "[AHCI CMD CRITICAL ERROR] Hardware Error reported in PxTFD: 0x%x\n", tfd);
            return 0;
        }

        if (--timeout == 0) {
            printk(LOG_DEBUG, "[AHCI CMD CRITICAL ERROR] Timeout waiting for command processing! PxTFD: 0x%x, PxCI: 0x%x\n", tfd, ci);
            return 0;
        }
    }

    return 1;
}

extern pci_device_t* devices;
extern uint32_t devicecount;

void init_ahci() {
    printk(LOG_DEBUG, "=================== AHCI INITIALIZATION LOG ===================\n");
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
        printk(LOG_DEBUG, "[AHCI INIT CRITICAL] No AHCI storage controller found on PCI bus.\n");
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

    printk(LOG_DEBUG, "[AHCI INIT] Resolved ABAR Physical Base Address: 0x%llx\n", abar_physical);

    page_table_t *pml4 = vmm_get_current_pml4();
    uint64_t abar_virtual = abar_physical + HHDM_OFFSET;
    vmm_map_page(pml4, abar_virtual, abar_physical, PTE_WRITABLE);
    
    volatile uint32_t pi = *(volatile uint32_t *)(abar_virtual + 0x0C); 
    printk(LOG_DEBUG, "[AHCI INIT] Implemented Ports Bitmask (PI): 0x%x\n", pi);

    for (int i = 0; i < 32; i++) {
        if (pi & (1 << i)) {
            uint64_t port_base = abar_virtual + 0x100 + (i * 0x80);
            
            volatile uint32_t ssts = *(volatile uint32_t *)(port_base + 0x28); 
            volatile uint32_t sig = *(volatile uint32_t *)(port_base + 0x24);
            uint8_t det = ssts & 0xF;
            
            printk(LOG_DEBUG, "[AHCI PORT %d] Status: PxSSTS=0x%x, PxSIG=0x%x, DET=0x%x\n", i, ssts, sig, det);

            if (det == 3) { 
                if (sig == AHCI_SIG_ATAPI) {
                    continue; 
                }
                if (sig != AHCI_SIG_SATA) {
                    continue;
                }

                printk(LOG_DEBUG, "[AHCI PORT %d] SATA HDD found. Allocating core memory mappings structures...\n", i);
                
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

                printk(LOG_DEBUG, "[AHCI PORT %d MEMORY]\n"
                                  "  -> Cmd List:  Virt=0x%llx, Phys=0x%llx\n"
                                  "  -> FIS Recv:   Virt=0x%llx, Phys=0x%llx\n"
                                  "  -> Cmd Table:  Virt=0x%llx, Phys=0x%llx\n"
                                  "  -> ID Buffer:   Virt=0x%llx, Phys=0x%llx\n", 
                       i, (uint64_t)cl_virt, cl_phys, (uint64_t)fis_virt, fis_phys, 
                       (uint64_t)table_virt, table_phys, (uint64_t)identify_buf_virt, identify_buf_phys);

                // Stop the port engines safely
                *(volatile uint32_t*)(port_base + 0x18) &= ~(1 << 0); 
                *(volatile uint32_t*)(port_base + 0x18) &= ~(1 << 4); 

                uint32_t timeout = 1000000;
                while((*(volatile uint32_t*)(port_base + 0x18) & (1 << 15)) || 
                      (*(volatile uint32_t*)(port_base + 0x18) & (1 << 14))) {
                    if (--timeout == 0) break;
                }

                // Write 64-bit physical addresses across low/high registers split cleanly
                *(volatile uint32_t*)(port_base + 0x00) = (uint32_t)cl_phys;         
                *(volatile uint32_t*)(port_base + 0x04) = (uint32_t)(cl_phys >> 32);  
                *(volatile uint32_t*)(port_base + 0x08) = (uint32_t)fis_phys;         
                *(volatile uint32_t*)(port_base + 0x0C) = (uint32_t)(fis_phys >> 32);  

                *(volatile uint32_t*)(port_base + 0x18) |= (1 << 4); 
                *(volatile uint32_t*)(port_base + 0x18) |= (1 << 0); 

                uint32_t final_pxcmd = *(volatile uint32_t*)(port_base + 0x18);
                printk(LOG_DEBUG, "[AHCI PORT %d] Port Engine Active. PxCMD: 0x%x\n", i, final_pxcmd);

                int cmd_success = ahci_send_command(port_base, cl_virt, table_virt, table_phys, 
                                                    0xEC, identify_buf_phys, 512, 0);

                if (!cmd_success) {
                    printk(LOG_DEBUG, "[AHCI PORT %d CRITICAL ERROR] IDENTIFY DEVICE failed!\n", i);
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
                
                primary_sata_drive.port_base = port_base;
                primary_sata_drive.cl_virt = cl_virt;
                primary_sata_drive.table_virt = table_virt;
                primary_sata_drive.table_phys = table_phys;
                primary_sata_drive.total_sectors = total_sectors;
                primary_sata_drive.size_gb = (uint32_t)drive_gb;
                primary_sata_drive.port_number = i;
                primary_sata_drive.is_initialized = 1;

                printk(LOG_DEBUG, "[AHCI] Drive Registered: Port %d, Total Sectors: %llu (%d GB).\n", 
                       i, total_sectors, primary_sata_drive.size_gb);

                // --- TEST DUMP: Verify structural reads work out safely via low mapping pointers ---
                void* test_block_virt = pmm_alloc_pages(0);
                uint64_t test_block_phys = (uint64_t)test_block_virt - HHDM_OFFSET;
                memset(test_block_virt, 0, PAGE_SIZE);

                printk(LOG_DEBUG, "[AHCI PORT %d TESTING] Dispatching read verification to LBA 0...\n", i);
                if (ahci_read_sectors(&primary_sata_drive, 0, 1, test_block_phys)) {
                    uint8_t* raw_bytes = (uint8_t*)test_block_virt;
                    printk(LOG_DEBUG, "[AHCI PORT %d TESTING] FIRST 8 BYTES OF LBA 0 (Hex Dump): ", i);
                    for(int b = 0; b < 8; b++) {
                        printk(LOG_DEBUG, "%x ", raw_bytes[b]);
                    }
                    printk(LOG_DEBUG, "\n");
                } else {
                    printk(LOG_DEBUG, "[AHCI PORT %d TESTING CRITICAL ERROR] Verification read on LBA 0 failed!\n", i);
                }
                break; 
            }
        }
    }
    printk(LOG_DEBUG, "===============================================================\n");
}

ahci_device_t* get_primary_sata_drive(void) {
    return primary_sata_drive.is_initialized ? &primary_sata_drive : NULL; 
}

int ahci_read_sectors(ahci_device_t* drive, uint64_t start_lba, uint16_t count, uint64_t buf_phys) {
    if (!drive || !drive->is_initialized) {
        printk(LOG_DEBUG, "[AHCI IO ERROR] Read attempt denied. Drive uninitialized.\n");
        return 0;
    }

    printk(LOG_DEBUG, "[AHCI READ DATA] LBA: %llu, Count: %u, Destination Phys: 0x%llx\n", start_lba, count, buf_phys);
    uint64_t port_base = drive->port_base;

    *(volatile uint32_t*)(port_base + 0x18) &= ~(1 << 0); 
    uint32_t timeout = 1000000;
    while(*(volatile uint32_t*)(port_base + 0x18) & (1 << 15)) {
        if (--timeout == 0) return 0;
    }

    ahci_cmd_header_t* cmd_hdr = (ahci_cmd_header_t*)drive->cl_virt;
    memset(cmd_hdr, 0, sizeof(ahci_cmd_header_t));
    cmd_hdr->cfl = 5;          
    cmd_hdr->w = 0;            
    cmd_hdr->prdtl = 1;        
    cmd_hdr->ctba = (uint32_t)drive->table_phys;
    cmd_hdr->ctbau = (uint32_t)(drive->table_phys >> 32);

    uint8_t* cmd_table = (uint8_t*)drive->table_virt;
    memset(cmd_table, 0, PAGE_SIZE);
    
    cmd_table[0] = 0x27;       // Register H2D FIS
    cmd_table[1] = 0x80;       // Command Execution
    cmd_table[2] = 0x25;       // READ DMA EXT Command code
    cmd_table[7] = 0x40;       // LBA Mode selection parameter

    cmd_table[4]  = (uint8_t)start_lba;          
    cmd_table[5]  = (uint8_t)(start_lba >> 8);   
    cmd_table[6]  = (uint8_t)(start_lba >> 16);  
    cmd_table[8]  = (uint8_t)(start_lba >> 24);  
    cmd_table[9]  = (uint8_t)(start_lba >> 32);  
    cmd_table[10] = (uint8_t)(start_lba >> 40);  

    cmd_table[12] = (uint8_t)count;             
    cmd_table[13] = (uint8_t)(count >> 8);      

    // Clean division of destination address into 32-bit fields
    ahci_prdt_entry_t* prdt = (ahci_prdt_entry_t*)(cmd_table + 0x80);
    prdt->dba = (uint32_t)buf_phys;
    prdt->dbau = (uint32_t)(buf_phys >> 32);
    prdt->dbc = ((uint32_t)count * 512) - 1;
    prdt->i = 1;

    printk(LOG_DEBUG, "[AHCI READ] Submitting PRDT entry -> DBA: 0x%x, DBAU: 0x%x\n", prdt->dba, prdt->dbau);

    *(volatile uint32_t*)(port_base + 0x18) |= (1 << 0); 
    *(volatile uint32_t*)(port_base + 0x38) = (1 << 0);  

    timeout = 5000000;
    while (1) {
        if ((*(volatile uint32_t*)(port_base + 0x38) & (1 << 0)) == 0) {
            break; 
        }
        uint32_t tfd = *(volatile uint32_t*)(port_base + 0x20);
        if (tfd & (1 << 0)) { 
            printk(LOG_DEBUG, "[AHCI READ CRITICAL ERROR] Hardware Error! TFD State: 0x%x\n", tfd);
            return 0;
        }
        if (--timeout == 0) {
            printk(LOG_DEBUG, "[AHCI READ CRITICAL ERROR] Transaction timed out!\n");
            return 0;
        }
    }

    return 1;
}

int ahci_write_sectors(ahci_device_t* drive, uint64_t start_lba, uint16_t count, uint64_t buf_phys) {
    if (!drive || !drive->is_initialized) {
        printk(LOG_DEBUG, "[AHCI IO ERROR] Write attempt denied. Drive uninitialized.\n");
        return 0;
    }

    printk(LOG_DEBUG, "[AHCI WRITE DATA] LBA: %llu, Count: %u, Source Phys: 0x%llx\n", start_lba, count, buf_phys);
    uint64_t port_base = drive->port_base;

    *(volatile uint32_t*)(port_base + 0x18) &= ~(1 << 0); 
    uint32_t timeout = 1000000;
    while(*(volatile uint32_t*)(port_base + 0x18) & (1 << 15)) {
        if (--timeout == 0) return 0;
    }

    ahci_cmd_header_t* cmd_hdr = (ahci_cmd_header_t*)drive->cl_virt;
    memset(cmd_hdr, 0, sizeof(ahci_cmd_header_t));
    cmd_hdr->cfl = 5;          
    cmd_hdr->w = 1;            
    cmd_hdr->prdtl = 1;        
    cmd_hdr->ctba = (uint32_t)drive->table_phys;
    cmd_hdr->ctbau = (uint32_t)(drive->table_phys >> 32);

    uint8_t* cmd_table = (uint8_t*)drive->table_virt;
    memset(cmd_table, 0, PAGE_SIZE);
    
    cmd_table[0] = 0x27;       
    cmd_table[1] = 0x80;       
    cmd_table[2] = 0x35;       // WRITE DMA EXT Command code
    cmd_table[7] = 0x40;       

    cmd_table[4]  = (uint8_t)start_lba;          
    cmd_table[5]  = (uint8_t)(start_lba >> 8);   
    cmd_table[6]  = (uint8_t)(start_lba >> 16);  
    cmd_table[8]  = (uint8_t)(start_lba >> 24);  
    cmd_table[9]  = (uint8_t)(start_lba >> 32);  
    cmd_table[10] = (uint8_t)(start_lba >> 40); 

    cmd_table[12] = (uint8_t)count;             
    cmd_table[13] = (uint8_t)(count >> 8);      

    ahci_prdt_entry_t* prdt = (ahci_prdt_entry_t*)(cmd_table + 0x80);
    prdt->dba = (uint32_t)buf_phys;
    prdt->dbau = (uint32_t)(buf_phys >> 32);
    prdt->dbc = ((uint32_t)count * 512) - 1;
    prdt->i = 1; 

    *(volatile uint32_t*)(port_base + 0x18) |= (1 << 0); 
    *(volatile uint32_t*)(port_base + 0x38) = (1 << 0);  

    timeout = 5000000;
    while (1) {
        if ((*(volatile uint32_t*)(port_base + 0x38) & (1 << 0)) == 0) {
            break; 
        }
        uint32_t tfd = *(volatile uint32_t*)(port_base + 0x20);
        if (tfd & (1 << 0)) { 
            printk(LOG_DEBUG, "[AHCI WRITE CRITICAL ERROR] Hardware Error! TFD State: 0x%x\n", tfd);
            return 0;
        }
        if (--timeout == 0) {
            printk(LOG_DEBUG, "[AHCI WRITE CRITICAL ERROR] Transaction timed out!\n");
            return 0;
        }
    }

    return 1;
}