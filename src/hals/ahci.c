#include <hals/pci.h>
#include <drivers/alloc.h>
#include <stdint.h>
#include <string.h>
#include <drivers/fb.h>
#include <hals/ahci.h>
#include <fs/mnt.h>
#include <errno.h>
#include <ioctl.h>
#define BIT(x) (1ULL << (x))

typedef uint64_t page_table_t;
extern page_table_t *vmm_get_current_pml4(void);
extern void vmm_map_page(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
extern void* pmm_alloc_pages(int order); 

static ahci_device_t primary_sata_drive;

ahci_device_t drives[32];
int lastd = 0;
// Helper function to safely stop a port's DMA engines
static void ahci_stop_port(uint64_t port_base) {
    printk(LOG_DEBUG, "[AHCI ENGINE] Attempting to stop port engines. PxCMD: 0x%x\n", *(volatile uint32_t*)(port_base + 0x18));
    
    *(volatile uint32_t*)(port_base + 0x18) &= ~(1 << 0); // Clear ST
    *(volatile uint32_t*)(port_base + 0x18) &= ~(1 << 4); // Clear FRE

    uint32_t timeout = 1000000;
    while (1) {
        uint32_t pxcmd = *(volatile uint32_t*)(port_base + 0x18);
        if (!(pxcmd & (1 << 15)) && !(pxcmd & (1 << 14))) {
            break;
        }
        if (--timeout == 0) {
            printk(LOG_DEBUG, "[AHCI ENGINE CRITICAL] Timeout waiting for port engines to idle! PxCMD: 0x%x\n", pxcmd);
            break;
        }
    }
    printk(LOG_DEBUG, "[AHCI ENGINE] Port engines stopped successfully. PxCMD: 0x%x\n", *(volatile uint32_t*)(port_base + 0x18));
}

// Helper function to safely start a port's DMA engines
static void ahci_start_port(uint64_t port_base) {
    printk(LOG_DEBUG, "[AHCI ENGINE] Attempting to start port engines. PxCMD: 0x%x\n", *(volatile uint32_t*)(port_base + 0x18));
    
    uint32_t timeout = 1000000;
    while (*(volatile uint32_t*)(port_base + 0x18) & (1 << 15)) {
        if (--timeout == 0) {
            printk(LOG_DEBUG, "[AHCI ENGINE CRITICAL] CR bit stuck active before start sequence!\n");
            break;
        }
    }

    *(volatile uint32_t*)(port_base + 0x18) |= (1 << 4); // Set FRE
    *(volatile uint32_t*)(port_base + 0x18) |= (1 << 0); // Set ST
    
    printk(LOG_DEBUG, "[AHCI ENGINE] Port engines command started. Current PxCMD: 0x%x\n", *(volatile uint32_t*)(port_base + 0x18));
}

int ahci_send_command(uint64_t port_base, void* cl_virt, void* table_virt, uint64_t table_phys, 
                      uint8_t ata_command, uint64_t buffer_phys, uint32_t buffer_size, int is_write) {
    
    printk(LOG_DEBUG, "[AHCI VERBOSE CMD] Executing raw command 0x%x. Buffer Phys: 0x%llx, Size: %u\n", 
           ata_command, buffer_phys, buffer_size);

    uint32_t initial_ci = *(volatile uint32_t*)(port_base + 0x38);
    uint32_t initial_tfd = *(volatile uint32_t*)(port_base + 0x20);
    printk(LOG_DEBUG, "[AHCI VERBOSE CMD] Initial registers -> PxCI: 0x%x, PxTFD: 0x%x\n", initial_ci, initial_tfd);

    if (initial_ci & (1 << 0)) {
        printk(LOG_DEBUG, "[AHCI CMD CRITICAL ERROR] Cannot dispatch; slot 0 is completely blocked by hardware.\n");
        return 0;
    }

    ahci_cmd_header_t* cmd_hdr = (ahci_cmd_header_t*)cl_virt;
    memset(cmd_hdr, 0, sizeof(ahci_cmd_header_t));
    cmd_hdr->cfl = 5;          
    cmd_hdr->w = is_write ? 1 : 0; 
    cmd_hdr->prdtl = 1;        
    cmd_hdr->ctba = (uint32_t)table_phys;
    cmd_hdr->ctbau = (uint32_t)(table_phys >> 32);

    uint8_t* cmd_table = (uint8_t*)table_virt;
    memset(cmd_table, 0, PAGE_SIZE); 
    cmd_table[0] = 0x27;       
    cmd_table[1] = 0x80;       
    cmd_table[2] = ata_command;
    cmd_table[7] = 0xA0;       

    ahci_prdt_entry_t* prdt = (ahci_prdt_entry_t*)(cmd_table + 0x80);
    prdt->dba = (uint32_t)buffer_phys;
    prdt->dbau = (uint32_t)(buffer_phys >> 32);
    prdt->dbc = buffer_size - 1; 
    prdt->i = 1;               
    
    printk(LOG_DEBUG, "[AHCI VERBOSE CMD] Slot 0 Setup complete. Header -> PRDTL: %d, CTBA: 0x%x\n", cmd_hdr->prdtl, cmd_hdr->ctba);

    // Fire the command execution processing bit
    *(volatile uint32_t*)(port_base + 0x38) = (1 << 0);  

    uint32_t timeout = 5000000;
    while (1) {
        uint32_t ci = *(volatile uint32_t*)(port_base + 0x38);
        uint32_t tfd = *(volatile uint32_t*)(port_base + 0x20);
        
        if ((ci & (1 << 0)) == 0) {
            printk(LOG_DEBUG, "[AHCI VERBOSE CMD] Command complete bit signaled by HBA. Final PxTFD: 0x%x\n", tfd);
            break; 
        }
        
        if (tfd & (1 << 0)) { 
            printk(LOG_DEBUG, "[AHCI CMD CRITICAL ERROR] Hardware execution failure! PxTFD: 0x%x, PxSERR: 0x%x\n", 
                   tfd, *(volatile uint32_t*)(port_base + 0x30));
            return 0;
        }

        if (--timeout == 0) {
            printk(LOG_DEBUG, "[AHCI CMD CRITICAL ERROR] Loop Timeout reached! PxCI: 0x%x, PxTFD: 0x%x\n", ci, tfd);
            return 0;
        }
    }

    return 1;
}

extern pci_device_t* devices;
extern uint32_t devicecount;
int ahci_read_sectors(ahci_device_t* drive, uint64_t start_lba, uint16_t count, uint64_t buf_phys);
int ahci_write_sectors(ahci_device_t* drive, uint64_t start_lba, uint16_t count, uint64_t buf_phys);
extern size_t buffer_crop_to_output(const void* src, size_t src_total_sz, 
                             void* dest, size_t offset, size_t count);
extern full_t fd_table[32];
#define PAGE_SHIFT 12
#define PAGE_MASK  (~((1ULL << PAGE_SHIFT) - 1))
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PTE_PRESENT  (1ULL << 0)
#define PTE_LARGE    (1ULL << 7)

// Extract 9-bit indices for each level
#define PML4_INDEX(va) (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)   (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)   (((va) >> 12) & 0x1FF)
#define PAGE_OFFSET(va) ((va) & 0xFFF)
/**
 * Inline assembly helper to read the CR3 register
 */
static inline uint64_t read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/**
 * Adjust this helper depending on your environment.
 * - Linux Kernel: use return (void*)__va(phys);
 * - Custom Kernel: use return (void*)(phys + PHYSICAL_MEMORY_OFFSET);
 */
static inline void* my_phys_to_virt(uint64_t phys) {
    // Replace 0xFFFF800000000000ULL with your kernel's direct mapping base address
    return (void*)(phys + HHDM_OFFSET);
}

/**
 * Walks the page tables to find the physical address of a virtual address.
 */
bool get_physical_address(uint64_t virt_addr, uint64_t *out_phys) {
    if (!out_phys) return false;

    // 1. Read CR3 and extract PML4 base physical address
    uint64_t cr3 = read_cr3();
    uint64_t pml4_phys = cr3 & PTE_ADDR_MASK;

    // 2. Walk PML4
    uint64_t *pml4 = (uint64_t*)my_phys_to_virt(pml4_phys);
    uint64_t pml4e = pml4[PML4_INDEX(virt_addr)];
    if (!(pml4e & PTE_PRESENT)) return false;

    // 3. Walk PDPT
    uint64_t pdpt_phys = pml4e & PTE_ADDR_MASK;
    uint64_t *pdpt = (uint64_t*)my_phys_to_virt(pdpt_phys);
    uint64_t pdpte = pdpt[PDPT_INDEX(virt_addr)];
    if (!(pdpte & PTE_PRESENT)) return false;

    // Handle 1GB Huge Page
    if (pdpte & PTE_LARGE) {
        *out_phys = (pdpte & 0xFFFFFC0000000ULL) + (virt_addr & 0x3FFFFFFFULL);
        return true;
    }

    // 4. Walk PD
    uint64_t pd_phys = pdpte & PTE_ADDR_MASK;
    uint64_t *pd = (uint64_t*)my_phys_to_virt(pd_phys);
    uint64_t pde = pd[PD_INDEX(virt_addr)];
    if (!(pde & PTE_PRESENT)) return false;

    // Handle 2MB Large Page
    if (pde & PTE_LARGE) {
        *out_phys = (pde & 0xFFFFFFFFFE000ULL) + (virt_addr & 0x1FFFFFULL);
        return true;
    }

    // 5. Walk PT
    uint64_t pt_phys = pde & PTE_ADDR_MASK;
    uint64_t *pt = (uint64_t*)my_phys_to_virt(pt_phys);
    uint64_t pte = pt[PT_INDEX(virt_addr)];
    if (!(pte & PTE_PRESENT)) return false;

    // 6. Final 4KB Page physical translation
    *out_phys = (pte & PTE_ADDR_MASK) + PAGE_OFFSET(virt_addr);
    return true;
}
size_t ahci_read(int fd, void* buf, size_t count, int offset) {
    // Since it's a block device read where offset is always 0, 
    // we start reading directly from LBA 0.
    int sector_count = count / 512;
    int remainder    = count % 512;   

    // Grab the raw device structure directly from your file descriptor table
    ahci_device_t* dev = &fd_table[fd].file.dev; 

    int i = 0;
    // 1. Read all full 512-byte sectors sequentially starting at LBA 0
    for (i = 0; i < sector_count; i++) {
        uint64_t phys;
        uintptr_t addr = (uintptr_t)buf + (i * 512);
        
        if (!get_physical_address(addr, &phys)) {
            return i * 512; // Return bytes read so far if translation fails
        }

        // Read directly from LBA index 'i'
        ahci_read_sectors(dev, i, 1, phys);
    }

    // 2. Handle the partial trailing bytes at the end of the block device read
    if (remainder != 0) {
        uint8_t bounce_buf[512] __attribute__((aligned(16))); 
        uint64_t bounce_phys;
        
        if (get_physical_address((uintptr_t)bounce_buf, &bounce_phys)) {
            // Read the next sequential LBA into the bounce buffer
            ahci_read_sectors(dev, i, 1, bounce_phys);
            
            // Copy only the exact requested bytes into the user's buffer
            uint8_t* dest = (uint8_t*)buf + (i * 512);
            for (int b = 0; b < remainder; b++) {
                dest[b] = bounce_buf[b];
            }
        }
    }

    return count;
}
size_t ahci_write(int fd, void* buf, size_t count) {
    // todo
    return -1;
}
int ahci_ioctl(int fd, unsigned long request, void* arg) {
    if (request == IOBLKSZ) {
        uint64_t* size = (uint64_t*)arg;
        *size = fd_table[fd].file.dev.total_sectors*512;
        return 0;
    } else if (request == IOBLKSC) {
        uint64_t* size = (uint64_t*)arg;
        *size = fd_table[fd].file.dev.total_sectors;
        return 0;
    }
    return -ENOTTY;
}
extern char *itoa(uint64_t value, char *str, int base, int uppercase);
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

                ahci_stop_port(port_base);

                *(volatile uint32_t*)(port_base + 0x00) = (uint32_t)cl_phys;         
                *(volatile uint32_t*)(port_base + 0x04) = (uint32_t)(cl_phys >> 32);  
                *(volatile uint32_t*)(port_base + 0x08) = (uint32_t)fis_phys;         
                *(volatile uint32_t*)(port_base + 0x0C) = (uint32_t)(fis_phys >> 32);  

                ahci_start_port(port_base);

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

                drives[lastd].port_base = port_base;
                drives[lastd].cl_virt = cl_virt;
                drives[lastd].table_virt = table_virt;
                drives[lastd].table_phys = table_phys;
                drives[lastd].total_sectors = total_sectors;
                drives[lastd].size_gb = (uint32_t)drive_gb;
                drives[lastd].port_number = i;
                drives[lastd].is_initialized = 1;

                printk(LOG_DEBUG, "[AHCI] Drive Registered: Port %d, Total Sectors: %llu (%d GB).\n", 
                       i, total_sectors, primary_sata_drive.size_gb);

                void* test_block_virt = pmm_alloc_pages(0);
                uint64_t test_block_phys = (uint64_t)test_block_virt - HHDM_OFFSET;
                memset(test_block_virt, 0, PAGE_SIZE);

                if (ahci_read_sectors(&primary_sata_drive, 0, 1, test_block_phys)) {
                    uint8_t* raw_bytes = (uint8_t*)test_block_virt;
                    printk(LOG_DEBUG, "[AHCI PORT %d TESTING] FIRST 8 BYTES OF LBA 0: ", i);
                    for(int b = 0; b < 8; b++) {
                        printk(LOG_DEBUG, "%02x ", raw_bytes[b]);
                    }
                    printk(LOG_DEBUG, "\n");
                }
                char name[6] = "/ada";
                char buf[2];
                itoa(lastd, buf, 10, false);
                strcat(name, buf);
                register_device((read_func_t)ahci_read, (ioctl_func_t)ahci_ioctl, (write_func_t)ahci_write, DISK_FLAGS, DISK, name, drives[lastd], 0);
                lastd++;
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
        printk(LOG_DEBUG, "[AHCI TRACE READ] Error: Drive unitialized.\n");
        return 0;
    }

    uint64_t port_base = drive->port_base;
    uint64_t target_phys = buf_phys;
    void* bounce_virt = NULL;
    uint64_t bounce_phys = 0;

    printk(LOG_DEBUG, "[AHCI TRACE READ] Request submitted. LBA: %llu, Sectors Count: %u, Orig Phys Address: 0x%llx\n", 
           start_lba, count, buf_phys);

    if (buf_phys & 0x3) {
        printk(LOG_DEBUG, "[AHCI ALIGNMENT WARNING] Physical address 0x%llx is NOT 4-byte aligned! Instantiating safe bounce buffer tracking pipeline...\n", buf_phys);
        bounce_virt = pmm_alloc_pages(0); 
        bounce_phys = (uint64_t)bounce_virt - HHDM_OFFSET;
        target_phys = bounce_phys;
        memset(bounce_virt, 0, PAGE_SIZE);
    }

    uint32_t current_ci = *(volatile uint32_t*)(port_base + 0x38);
    if (current_ci & (1 << 0)) {
        printk(LOG_DEBUG, "[AHCI TRACE READ CRITICAL] Slot 0 is stuck busy before execution! PxCI: 0x%x\n", current_ci);
        return 0;
    }

    // Clear interrupt status and error registers to ensure a clean slate
    *(volatile uint32_t*)(port_base + 0x10) = 0xFFFFFFFF;
    *(volatile uint32_t*)(port_base + 0x30) = 0xFFFFFFFF;

    ahci_cmd_header_t* cmd_hdr = (ahci_cmd_header_t*)drive->cl_virt;
    memset(cmd_hdr, 0, sizeof(ahci_cmd_header_t));
    cmd_hdr->cfl = 5;          
    cmd_hdr->w = 0;          
    cmd_hdr->prdtl = 1;        
    cmd_hdr->ctba = (uint32_t)drive->table_phys;
    cmd_hdr->ctbau = (uint32_t)(drive->table_phys >> 32);

    uint8_t* cmd_table = (uint8_t*)drive->table_virt;
    memset(cmd_table, 0, PAGE_SIZE);
    
    cmd_table[0] = 0x27;       
    cmd_table[1] = 0x80;       
    cmd_table[2] = 0x25;       // READ DMA EXT
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
    prdt->dba = (uint32_t)target_phys;
    prdt->dbau = (uint32_t)(target_phys >> 32);
    prdt->dbc = ((uint32_t)count * 512) - 1;
    prdt->i = 1;

    *(volatile uint32_t*)(port_base + 0x38) = (1 << 0);  

    uint32_t timeout = 5000000;
    while (1) {
        uint32_t ci = *(volatile uint32_t*)(port_base + 0x38);
        uint32_t tfd = *(volatile uint32_t*)(port_base + 0x20);
        if ((ci & (1 << 0)) == 0) {
            break; 
        }
        if (tfd & (1 << 0)) { 
            printk(LOG_DEBUG, "[AHCI TRACE READ CRITICAL] Hardware device failure flag! PxTFD: 0x%x\n", tfd);
            if (bounce_virt) pmm_free_pages(bounce_virt, 0); // avoid memory leaks
            return 0;
        }
        if (--timeout == 0) {
            printk(LOG_DEBUG, "[AHCI TRACE READ CRITICAL] Transaction timed out! PxCI: 0x%x, PxTFD: 0x%x\n", ci, tfd);
            if (bounce_virt) pmm_free_pages(bounce_virt, 0);
            return 0;
        }
    }

    if (bounce_virt != NULL) {
        void* dest_virt = (void*)(buf_phys + HHDM_OFFSET);
        memcpy(dest_virt, bounce_virt, (uint32_t)count * 512);
        // Clean up unaligned allocation page tracking
        // pmm_free_pages(bounce_virt, 0); 
    }

    return 1;
}

int ahci_write_sectors(ahci_device_t* drive, uint64_t start_lba, uint16_t count, uint64_t buf_phys) {
    if (!drive || !drive->is_initialized) {
        printk(LOG_DEBUG, "[AHCI TRACE WRITE] Error: Drive uninitialized.\n");
        return 0;
    }

    uint64_t port_base = drive->port_base;
    uint64_t target_phys = buf_phys;
    void* bounce_virt = NULL;
    uint64_t bounce_phys = 0;

    if (buf_phys & 0x3) {
        printk(LOG_DEBUG, "[AHCI ALIGNMENT WARNING] Physical write address 0x%llx is NOT 4-byte aligned! Generating bounce tracking configuration...\n", buf_phys);
        bounce_virt = pmm_alloc_pages(0);
        bounce_phys = (uint64_t)bounce_virt - HHDM_OFFSET;
        void* src_virt = (void*)(buf_phys + HHDM_OFFSET);
        memcpy(bounce_virt, src_virt, (uint32_t)count * 512);
        target_phys = bounce_phys;
    }

    if (*(volatile uint32_t*)(port_base + 0x38) & (1 << 0)) {
        printk(LOG_DEBUG, "[AHCI TRACE WRITE CRITICAL] Port engine active slot 0 busy block.\n");
        if (bounce_virt) pmm_free_pages(bounce_virt, 0);
        return 0;
    }

    // FIX: Clear interrupt status and error codes before triggering command execution pipeline
    *(volatile uint32_t*)(port_base + 0x10) = 0xFFFFFFFF; 
    *(volatile uint32_t*)(port_base + 0x30) = 0xFFFFFFFF;

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
    cmd_table[2] = 0x35;       // WRITE DMA EXT
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
    prdt->dba = (uint32_t)target_phys;
    prdt->dbau = (uint32_t)(target_phys >> 32);
    prdt->dbc = ((uint32_t)count * 512) - 1;
    prdt->i = 1; 

    *(volatile uint32_t*)(port_base + 0x38) = (1 << 0);  

    uint32_t timeout = 5000000;
    while (1) {
        uint32_t ci = *(volatile uint32_t*)(port_base + 0x38);
        uint32_t tfd = *(volatile uint32_t*)(port_base + 0x20);
        if ((ci & (1 << 0)) == 0) {
            break; 
        }
        if (tfd & (1 << 0)) { 
            printk(LOG_DEBUG, "[AHCI TRACE WRITE CRITICAL] Hardware Error! TFD State: 0x%x\n", tfd);
            if (bounce_virt) pmm_free_pages(bounce_virt, 0);
            return 0;
        }
        if (--timeout == 0) {
            printk(LOG_DEBUG, "[AHCI TRACE WRITE CRITICAL] Transaction timed out!\n");
            if (bounce_virt) pmm_free_pages(bounce_virt, 0);
            return 0;
        }
    }

    // Clean up bounce page if allocated
    // if (bounce_virt) pmm_free_pages(bounce_virt, 0);

    return 1;
}
