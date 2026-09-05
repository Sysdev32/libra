#include <hals/nvme.h>
#include <hals/pci.h>
#include <drivers/fb.h>
#include <string.h>

#define PAGE_SIZE 4096
#define HHDM_OFFSET 0xffff800000000000ULL

#define PTE_PRESENT       (1ULL << 0)
#define PTE_WRITABLE      (1ULL << 1)
#define PTE_WRITE_THROUGH (1ULL << 3)
#define PTE_CACHE_DISABLE (1ULL << 4)

typedef uint64_t page_table_t;
extern pci_device_t* devices;
extern uint32_t devicecount;
extern page_table_t *vmm_get_current_pml4(void);
extern void vmm_map_page(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
extern void *pmm_alloc_pages(int order);
extern void pmm_free_pages(void *ptr, int order);

static nvme_device_t nvme_devices[NVME_MAX_DEVICES] = {0};
static uint32_t num_nvme_devices = 0;

/* ============================================================================
 * HAL FUNCTIONS (DMA & MMIO MAPPING)
 * ============================================================================ */

static void* hal_alloc_dma_page(void) {
    void* ptr = pmm_alloc_pages(0);
    if (ptr) {
        memset(ptr, 0, PAGE_SIZE);
        printk(LOG_TRACE, "NVMe [HAL]: Allocated DMA page at virt %p\n", ptr);
    } else {
        printk(LOG_ERROR, "NVMe [HAL]: DMA page allocation failed!\n");
    }
    return ptr;
}

static void hal_free_dma_page(void* ptr) {
    if (ptr) {
        printk(LOG_TRACE, "NVMe [HAL]: Freeing DMA page at virt %p\n", ptr);
        pmm_free_pages(ptr, 0);
    }
}

static uintptr_t hal_virt_to_phys(void* virt) {
    uintptr_t vaddr = (uintptr_t)virt;
    if (vaddr >= HHDM_OFFSET) {
        return vaddr - HHDM_OFFSET;
    }
    return vaddr; 
}

static void* hal_mmio_map(uintptr_t phys_addr, size_t size) {
    page_table_t *pml4 = vmm_get_current_pml4();
    
    uint64_t virt_start = phys_addr + HHDM_OFFSET;
    uint64_t phys_page  = phys_addr & ~(PAGE_SIZE - 1);
    uint64_t offset     = phys_addr & (PAGE_SIZE - 1);
    uint64_t num_pages  = ((uint64_t)size + offset + PAGE_SIZE - 1) / PAGE_SIZE;

    printk(LOG_TRACE, "NVMe [HAL]: Mapping MMIO Phys 0x%llx -> Virt 0x%llx (%d pages)\n", 
           (unsigned long long)phys_addr, (unsigned long long)(virt_start + offset), (int)num_pages);

    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t curr_phys = phys_page + (i * PAGE_SIZE);
        uint64_t curr_virt = (virt_start & ~(PAGE_SIZE - 1)) + (i * PAGE_SIZE);
        
        uint64_t flags = PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE | PTE_WRITE_THROUGH;
        vmm_map_page(pml4, curr_virt, curr_phys, flags);
        asm volatile("invlpg (%0)" :: "r"(curr_virt) : "memory");
    }

    return (void *)(virt_start);
}

/* ============================================================================
 * INTERNAL HELPER & IDENTIFY FUNCTIONS
 * ============================================================================ */

static inline void nvme_ring_doorbell(nvme_device_t* ctrl, uint32_t doorbell_index, uint32_t val) {
    volatile uint32_t* db_addr = (volatile uint32_t*)(
        (uintptr_t)ctrl->regs + 0x1000 + (doorbell_index * ctrl->db_stride)
    );
    *db_addr = val;
}

static bool nvme_submit_admin_cmd(nvme_device_t* ctrl, nvme_sqe_t* cmd, nvme_cqe_t* out_cqe) {
    cmd->command_id = ++ctrl->cmd_id_counter;

    memcpy(&ctrl->admin_sq[ctrl->admin_sq_tail], cmd, sizeof(nvme_sqe_t));
    ctrl->admin_sq_tail = (ctrl->admin_sq_tail + 1) % QUEUE_DEPTH;
    
    // Admin SQ Doorbell = Index 0
    nvme_ring_doorbell(ctrl, 0, ctrl->admin_sq_tail);

    volatile nvme_cqe_t* cqe = (volatile nvme_cqe_t*)&ctrl->admin_cq[ctrl->admin_cq_head];
    uint32_t spin_count = 0;
    while ((cqe->status & 1) != ctrl->admin_phase) {
        spin_count++;
        asm volatile("pause" ::: "memory");
        if (spin_count % 10000000 == 0) {
            printk(LOG_WARNING, "NVMe [%d] [AdminCQ]: Waiting for completion (spins: %d, csts: 0x%x)...\n", 
                   ctrl->id, spin_count, ctrl->regs->csts);
        }
        if (spin_count > 100000000) {
            printk(LOG_ERROR, "NVMe [%d] [AdminCQ]: Timeout waiting for command 0x%x!\n", ctrl->id, cmd->opcode);
            return false;
        }
    }

    if (out_cqe) *out_cqe = *(nvme_cqe_t*)cqe;

    ctrl->admin_cq_head = (ctrl->admin_cq_head + 1) % QUEUE_DEPTH;
    if (ctrl->admin_cq_head == 0) {
        ctrl->admin_phase ^= 1;
    }

    // Admin CQ Doorbell = Index 1
    nvme_ring_doorbell(ctrl, 1, ctrl->admin_cq_head);

    uint16_t status_code = cqe->status >> 1;
    if (status_code != 0) {
        printk(LOG_ERROR, "NVMe [%d] [AdminSQ]: Cmd opcode 0x%x failed with status 0x%x!\n", 
               ctrl->id, cmd->opcode, status_code);
        return false;
    }

    return true;
}

static void string_trim(char* str, size_t len) {
    while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\0' || str[len - 1] == '\r' || str[len - 1] == '\n')) {
        str[--len] = '\0';
    }
}

static bool nvme_identify(nvme_device_t* ctrl) {
    void* dma_buf = hal_alloc_dma_page();
    if (!dma_buf) return false;

    uint64_t phys_buf = (uint64_t)hal_virt_to_phys(dma_buf);

    // --- 1. IDENTIFY CONTROLLER (CNS = 1) ---
    nvme_sqe_t cmd = {0};
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.prp1   = phys_buf;
    cmd.cdw10  = 1;    // Controller structure (CNS = 1)

    if (!nvme_submit_admin_cmd(ctrl, &cmd, NULL)) {
        printk(LOG_ERROR, "NVMe [%d]: Identify Controller command failed!\n", ctrl->id);
        hal_free_dma_page(dma_buf);
        return false;
    }

    nvme_identify_ctrl_t* id_ctrl = (nvme_identify_ctrl_t*)dma_buf;

    char model[41] = {0};
    char serial[21] = {0};
    char firmware[9] = {0};
    memcpy(model, id_ctrl->mn, 40);
    memcpy(serial, id_ctrl->sn, 20);
    memcpy(firmware, id_ctrl->fr, 8);

    string_trim(model, 40);
    string_trim(serial, 20);
    string_trim(firmware, 8);

    printk(LOG_INFO, "================ NVMe DRIVE DETAILS ================\n");
    printk(LOG_INFO, "Model       : %s\n", model);
    printk(LOG_INFO, "Serial      : %s\n", serial);
    printk(LOG_INFO, "Firmware    : %s\n", firmware);
    printk(LOG_INFO, "Vendor ID   : 0x%04x\n", id_ctrl->vid);
    printk(LOG_INFO, "Namespaces  : %u\n", id_ctrl->nn);

    // --- 2. IDENTIFY NAMESPACE 1 (CNS = 0) ---
    memset(dma_buf, 0, PAGE_SIZE);
    memset(&cmd, 0, sizeof(nvme_sqe_t));

    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid   = 1;    // Target Namespace ID 1
    cmd.prp1   = phys_buf;
    cmd.cdw10  = 0;    // Namespace structure (CNS = 0)

    if (!nvme_submit_admin_cmd(ctrl, &cmd, NULL)) {
        printk(LOG_ERROR, "NVMe [%d]: Identify Namespace 1 failed!\n", ctrl->id);
        hal_free_dma_page(dma_buf);
        return false;
    }

    nvme_identify_ns_t* id_ns = (nvme_identify_ns_t*)dma_buf;

    // Extract active LBA format details
    uint8_t active_lbaf = id_ns->flbas & 0x0F;
    uint8_t lbads = id_ns->lbaf[active_lbaf].lbads;

    // Fallback if field was clear or invalid
    if (lbads < 9) {
        lbads = 9; 
    }

    uint32_t block_size   = 1U << lbads;
    uint64_t total_blocks = id_ns->nsze;
    uint64_t total_bytes  = total_blocks * block_size;
    uint64_t size_mb      = total_bytes / (1024 * 1024);
    uint64_t size_gb      = size_mb / 1024;

    ctrl->nsid          = 1;
    ctrl->sector_size   = block_size;
    ctrl->total_sectors = total_blocks;

    printk(LOG_INFO, "--- Namespace 1 Info ---\n");
    printk(LOG_INFO, "Block Size  : %u bytes (2^%u)\n", block_size, lbads);
    printk(LOG_INFO, "Total LBAs  : %llu\n", (unsigned long long)total_blocks);
    if (size_gb > 0) {
        printk(LOG_INFO, "Capacity    : %llu GB (%llu MB)\n", (unsigned long long)size_gb, (unsigned long long)size_mb);
    } else {
        printk(LOG_INFO, "Capacity    : %llu MB\n", (unsigned long long)size_mb);
    }
    printk(LOG_INFO, "====================================================\n");

    hal_free_dma_page(dma_buf);
    return true;
}

static bool nvme_init_single_device(nvme_device_t* ctrl, pci_device_t* pci_dev, uint32_t id) {
    ctrl->id = id;

    // 1. Enable PCI Bus Mastering, Memory, and IO Space
    uint16_t pci_cmd = pci_read16(pci_dev->bus, pci_dev->device, pci_dev->function, 0x04);
    pci_cmd |= (1 << 0) | (1 << 1) | (1 << 2);
    pci_write16(pci_dev->bus, pci_dev->device, pci_dev->function, 0x04, pci_cmd);

    // 2. Read full 64-bit BAR0
    uint32_t bar0_low  = pci_read32(pci_dev->bus, pci_dev->device, pci_dev->function, 0x10);
    uint32_t bar0_high = pci_read32(pci_dev->bus, pci_dev->device, pci_dev->function, 0x14);

    ctrl->bar0 = ((uint64_t)bar0_high << 32) | ((uint64_t)bar0_low & 0xFFFFFFF0ULL);

    ctrl->regs = (volatile nvme_regs_t*)hal_mmio_map(ctrl->bar0, 0x2000);
    if (!ctrl->regs) {
        return false;
    }

    uint32_t dstrd = (uint32_t)((ctrl->regs->cap >> 32) & 0x0F);
    ctrl->db_stride = 1 << (2 + dstrd);

    // 3. Reset Controller (CC.EN = 0)
    ctrl->regs->cc &= ~1U;
    while ((ctrl->regs->csts & 1) != 0) {
        asm volatile("pause");
    }

    // 4. Allocate Admin Queues
    ctrl->admin_sq = (nvme_sqe_t*)hal_alloc_dma_page();
    ctrl->admin_cq = (nvme_cqe_t*)hal_alloc_dma_page();

    if (!ctrl->admin_sq || !ctrl->admin_cq) {
        return false;
    }

    ctrl->admin_sq_tail = 0;
    ctrl->admin_cq_head = 0;
    ctrl->admin_phase   = 1;

    // 0-based depth for AQA register
    uint32_t aqa = ((QUEUE_DEPTH - 1) << 16) | (QUEUE_DEPTH - 1);
    uint64_t asq_phys = (uint64_t)hal_virt_to_phys(ctrl->admin_sq);
    uint64_t acq_phys = (uint64_t)hal_virt_to_phys(ctrl->admin_cq);

    ctrl->regs->aqa = aqa;
    ctrl->regs->asq = asq_phys;
    ctrl->regs->acq = acq_phys;

    // 5. Enable Controller (CC.EN = 1)
    uint32_t cc = 0;
    cc |= (1 << 0);   // EN = 1
    cc |= (6 << 16);  // IOSQES = 64 bytes
    cc |= (4 << 20);  // IOCQES = 16 bytes
    ctrl->regs->cc = cc;

    while ((ctrl->regs->csts & 1) == 0) {
        asm volatile("pause");
    }

    // 6. Create I/O Completion Queue (QID = 1)
    ctrl->io_cq = (nvme_cqe_t*)hal_alloc_dma_page();
    if (!ctrl->io_cq) return false;

    uint64_t io_cq_phys = (uint64_t)hal_virt_to_phys(ctrl->io_cq);

    nvme_sqe_t cmd_cq = {0};
    cmd_cq.opcode = NVME_ADMIN_CREATE_IO_CQ;
    cmd_cq.prp1   = io_cq_phys;
    cmd_cq.cdw10  = ((QUEUE_DEPTH - 1) << 16) | 1; 
    cmd_cq.cdw11  = 1;                             
    if (!nvme_submit_admin_cmd(ctrl, &cmd_cq, NULL)) {
        printk(LOG_ERROR, "NVMe [%d]: Failed to create I/O Completion Queue!\n", ctrl->id);
        return false;
    }

    // 7. Create I/O Submission Queue (QID = 1)
    ctrl->io_sq = (nvme_sqe_t*)hal_alloc_dma_page();
    if (!ctrl->io_sq) return false;

    uint64_t io_sq_phys = (uint64_t)hal_virt_to_phys(ctrl->io_sq);

    nvme_sqe_t cmd_sq = {0};
    cmd_sq.opcode = NVME_ADMIN_CREATE_IO_SQ;
    cmd_sq.prp1   = io_sq_phys;
    cmd_sq.cdw10  = ((QUEUE_DEPTH - 1) << 16) | 1; 
    cmd_sq.cdw11  = (1 << 16) | 1;                 
    if (!nvme_submit_admin_cmd(ctrl, &cmd_sq, NULL)) {
        printk(LOG_ERROR, "NVMe [%d]: Failed to create I/O Submission Queue!\n", ctrl->id);
        return false;
    }

    ctrl->io_sq_tail = 0;
    ctrl->io_cq_head = 0;
    ctrl->io_phase   = 1;
    ctrl->active     = true;

    // Fetch drive and namespace details
    nvme_identify(ctrl);

    return true;
}

/* ============================================================================
 * PUBLIC DRIVER APIS
 * ============================================================================ */
int32_t nvme_init(uint8_t bus, uint8_t dev, uint8_t func) {
    if (num_nvme_devices >= NVME_MAX_DEVICES) {
        printk(LOG_ERROR, "NVMe [HAL]: Max device limit reached (%d)\n", NVME_MAX_DEVICES);
        return -1;
    }

    uint32_t id = num_nvme_devices;
    nvme_device_t* ctrl = &nvme_devices[id];
    memset(ctrl, 0, sizeof(nvme_device_t));
    ctrl->id = id;

    // 1. Enable PCI Bus Mastering, Memory, and IO Space
    uint16_t pci_cmd = pci_read16(bus, dev, func, 0x04);
    pci_cmd |= (1 << 0) | (1 << 1) | (1 << 2);
    pci_write16(bus, dev, func, 0x04, pci_cmd);

    // 2. Read full 64-bit BAR0
    uint32_t bar0_low  = pci_read32(bus, dev, func, 0x10);
    uint32_t bar0_high = pci_read32(bus, dev, func, 0x14);

    ctrl->bar0 = ((uint64_t)bar0_high << 32) | ((uint64_t)bar0_low & 0xFFFFFFF0ULL);

    ctrl->regs = (volatile nvme_regs_t*)hal_mmio_map(ctrl->bar0, 0x2000);
    if (!ctrl->regs) {
        return -1;
    }

    uint32_t dstrd = (uint32_t)((ctrl->regs->cap >> 32) & 0x0F);
    ctrl->db_stride = 1 << (2 + dstrd);

    // 3. Reset Controller (CC.EN = 0)
    ctrl->regs->cc &= ~1U;
    while ((ctrl->regs->csts & 1) != 0) {
        asm volatile("pause");
    }

    // 4. Allocate Admin Queues
    ctrl->admin_sq = (nvme_sqe_t*)hal_alloc_dma_page();
    ctrl->admin_cq = (nvme_cqe_t*)hal_alloc_dma_page();

    if (!ctrl->admin_sq || !ctrl->admin_cq) {
        return -1;
    }

    ctrl->admin_sq_tail = 0;
    ctrl->admin_cq_head = 0;
    ctrl->admin_phase   = 1;

    // 0-based depth for AQA register
    uint32_t aqa = ((QUEUE_DEPTH - 1) << 16) | (QUEUE_DEPTH - 1);
    uint64_t asq_phys = (uint64_t)hal_virt_to_phys(ctrl->admin_sq);
    uint64_t acq_phys = (uint64_t)hal_virt_to_phys(ctrl->admin_cq);

    ctrl->regs->aqa = aqa;
    ctrl->regs->asq = asq_phys;
    ctrl->regs->acq = acq_phys;

    // 5. Enable Controller (CC.EN = 1)
    uint32_t cc = 0;
    cc |= (1 << 0);   // EN = 1
    cc |= (6 << 16);  // IOSQES = 64 bytes
    cc |= (4 << 20);  // IOCQES = 16 bytes
    ctrl->regs->cc = cc;

    while ((ctrl->regs->csts & 1) == 0) {
        asm volatile("pause");
    }

    // 6. Create I/O Completion Queue (QID = 1)
    ctrl->io_cq = (nvme_cqe_t*)hal_alloc_dma_page();
    if (!ctrl->io_cq) return -1;

    uint64_t io_cq_phys = (uint64_t)hal_virt_to_phys(ctrl->io_cq);

    nvme_sqe_t cmd_cq = {0};
    cmd_cq.opcode = NVME_ADMIN_CREATE_IO_CQ;
    cmd_cq.prp1   = io_cq_phys;
    cmd_cq.cdw10  = ((QUEUE_DEPTH - 1) << 16) | 1;
    cmd_cq.cdw11  = 1;
    if (!nvme_submit_admin_cmd(ctrl, &cmd_cq, NULL)) {
        printk(LOG_ERROR, "NVMe [%d]: Failed to create I/O Completion Queue!\n", ctrl->id);
        return -1;
    }

    // 7. Create I/O Submission Queue (QID = 1)
    ctrl->io_sq = (nvme_sqe_t*)hal_alloc_dma_page();
    if (!ctrl->io_sq) return -1;

    uint64_t io_sq_phys = (uint64_t)hal_virt_to_phys(ctrl->io_sq);

    nvme_sqe_t cmd_sq = {0};
    cmd_sq.opcode = NVME_ADMIN_CREATE_IO_SQ;
    cmd_sq.prp1   = io_sq_phys;
    cmd_sq.cdw10  = ((QUEUE_DEPTH - 1) << 16) | 1;
    cmd_sq.cdw11  = (1 << 16) | 1;
    if (!nvme_submit_admin_cmd(ctrl, &cmd_sq, NULL)) {
        printk(LOG_ERROR, "NVMe [%d]: Failed to create I/O Submission Queue!\n", ctrl->id);
        return -1;
    }

    ctrl->io_sq_tail = 0;
    ctrl->io_cq_head = 0;
    ctrl->io_phase   = 1;
    ctrl->active     = true;

    // Fetch drive and namespace details
    if (!nvme_identify(ctrl)) {
        return -1;
    }

    num_nvme_devices++;
    return (int32_t)id;
}
bool nvme_read_block(uint32_t nvme_id, uint32_t nsid, uint64_t lba, uint16_t sector_count, void* buffer) {
    if (nvme_id >= num_nvme_devices || !nvme_devices[nvme_id].active || !buffer) {
        return false;
    }

    nvme_device_t* ctrl = &nvme_devices[nvme_id];
    uint64_t phys_buf = (uint64_t)hal_virt_to_phys(buffer);
    uint32_t total_bytes = sector_count * ctrl->sector_size;

    // Calculate how many physical 4 KiB pages are spanned
    uint64_t page_offset = phys_buf & (PAGE_SIZE - 1);
    uint32_t bytes_in_first_page = PAGE_SIZE - page_offset;

    void* prp_list_virt = NULL;

    nvme_sqe_t cmd = {0};
    cmd.opcode     = NVME_CMD_READ;
    cmd.command_id = ++ctrl->cmd_id_counter;
    cmd.nsid       = nsid;
    cmd.prp1       = phys_buf;
    cmd.cdw10      = (uint32_t)(lba & 0xFFFFFFFF);
    cmd.cdw11      = (uint32_t)(lba >> 32);
    cmd.cdw12      = (sector_count - 1);

    // Multi-page transfer setup
    if (total_bytes > bytes_in_first_page) {
        uint32_t remaining_bytes = total_bytes - bytes_in_first_page;
        uint64_t second_page_phys = phys_buf + bytes_in_first_page;

        if (remaining_bytes <= PAGE_SIZE) {
            // Exactly 2 pages: PRP2 points directly to the 2nd physical page
            cmd.prp2 = second_page_phys;
        } else {
            // Spans > 2 pages: Allocate a PRP List page
            prp_list_virt = hal_alloc_dma_page();
            if (!prp_list_virt) return false;

            uint64_t* prp_list = (uint64_t*)prp_list_virt;
            uint32_t num_pages = (remaining_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

            for (uint32_t i = 0; i < num_pages; i++) {
                prp_list[i] = second_page_phys + (i * PAGE_SIZE);
            }

            cmd.prp2 = (uint64_t)hal_virt_to_phys(prp_list_virt);
        }
    }

    memcpy(&ctrl->io_sq[ctrl->io_sq_tail], &cmd, sizeof(nvme_sqe_t));
    ctrl->io_sq_tail = (ctrl->io_sq_tail + 1) % QUEUE_DEPTH;

    nvme_ring_doorbell(ctrl, 2, ctrl->io_sq_tail);

    volatile nvme_cqe_t* cqe = (volatile nvme_cqe_t*)&ctrl->io_cq[ctrl->io_cq_head];
    uint32_t spin_count = 0;
    while ((cqe->status & 1) != ctrl->io_phase) {
        asm volatile("pause" ::: "memory");
        if (++spin_count > 100000000) {
            printk(LOG_ERROR, "NVMe [%d]: Read timeout!\n", ctrl->id);
            if (prp_list_virt) hal_free_dma_page(prp_list_virt);
            return false;
        }
    }

    ctrl->io_cq_head = (ctrl->io_cq_head + 1) % QUEUE_DEPTH;
    if (ctrl->io_cq_head == 0) ctrl->io_phase ^= 1;

    nvme_ring_doorbell(ctrl, 3, ctrl->io_cq_head);

    if (prp_list_virt) {
        hal_free_dma_page(prp_list_virt);
    }

    return (cqe->status >> 1) == 0;
}

bool nvme_write_block(uint32_t nvme_id, uint32_t nsid, uint64_t lba, uint16_t sector_count, const void* buffer) {
    if (nvme_id >= num_nvme_devices || !nvme_devices[nvme_id].active || !buffer) {
        return false;
    }

    nvme_device_t* ctrl = &nvme_devices[nvme_id];
    uint64_t phys_buf = (uint64_t)hal_virt_to_phys((void*)buffer);
    uint32_t total_bytes = sector_count * ctrl->sector_size;

    uint64_t page_offset = phys_buf & (PAGE_SIZE - 1);
    uint32_t bytes_in_first_page = PAGE_SIZE - page_offset;

    void* prp_list_virt = NULL;

    nvme_sqe_t cmd = {0};
    cmd.opcode     = NVME_CMD_WRITE;
    cmd.command_id = ++ctrl->cmd_id_counter;
    cmd.nsid       = nsid;
    cmd.prp1       = phys_buf;
    cmd.cdw10      = (uint32_t)(lba & 0xFFFFFFFF);
    cmd.cdw11      = (uint32_t)(lba >> 32);
    cmd.cdw12      = (sector_count - 1);

    if (total_bytes > bytes_in_first_page) {
        uint32_t remaining_bytes = total_bytes - bytes_in_first_page;
        uint64_t second_page_phys = phys_buf + bytes_in_first_page;

        if (remaining_bytes <= PAGE_SIZE) {
            cmd.prp2 = second_page_phys;
        } else {
            prp_list_virt = hal_alloc_dma_page();
            if (!prp_list_virt) return false;

            uint64_t* prp_list = (uint64_t*)prp_list_virt;
            uint32_t num_pages = (remaining_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

            for (uint32_t i = 0; i < num_pages; i++) {
                prp_list[i] = second_page_phys + (i * PAGE_SIZE);
            }

            cmd.prp2 = (uint64_t)hal_virt_to_phys(prp_list_virt);
        }
    }

    memcpy(&ctrl->io_sq[ctrl->io_sq_tail], &cmd, sizeof(nvme_sqe_t));
    ctrl->io_sq_tail = (ctrl->io_sq_tail + 1) % QUEUE_DEPTH;

    nvme_ring_doorbell(ctrl, 2, ctrl->io_sq_tail);

    volatile nvme_cqe_t* cqe = (volatile nvme_cqe_t*)&ctrl->io_cq[ctrl->io_cq_head];
    uint32_t spin_count = 0;
    while ((cqe->status & 1) != ctrl->io_phase) {
        asm volatile("pause" ::: "memory");
        if (++spin_count > 100000000) {
            printk(LOG_ERROR, "NVMe [%d]: Write timeout!\n", ctrl->id);
            if (prp_list_virt) hal_free_dma_page(prp_list_virt);
            return false;
        }
    }

    ctrl->io_cq_head = (ctrl->io_cq_head + 1) % QUEUE_DEPTH;
    if (ctrl->io_cq_head == 0) ctrl->io_phase ^= 1;

    nvme_ring_doorbell(ctrl, 3, ctrl->io_cq_head);

    if (prp_list_virt) {
        hal_free_dma_page(prp_list_virt);
    }

    return (cqe->status >> 1) == 0;
}

void nvme_close(void) {
    for (uint32_t i = 0; i < num_nvme_devices; i++) {
        nvme_device_t* ctrl = &nvme_devices[i];
        if (!ctrl->active) continue;

        if (ctrl->regs) {
            ctrl->regs->cc &= ~1U;
            while ((ctrl->regs->csts & 1) != 0) {
                asm volatile("pause");
            }
            ctrl->regs = NULL;
        }

        if (ctrl->admin_sq) { hal_free_dma_page(ctrl->admin_sq); ctrl->admin_sq = NULL; }
        if (ctrl->admin_cq) { hal_free_dma_page(ctrl->admin_cq); ctrl->admin_cq = NULL; }
        if (ctrl->io_sq)    { hal_free_dma_page(ctrl->io_sq);    ctrl->io_sq = NULL; }
        if (ctrl->io_cq)    { hal_free_dma_page(ctrl->io_cq);    ctrl->io_cq = NULL; }

        ctrl->active = false;
    }

    num_nvme_devices = 0;
}

int32_t nvme_get_namespaces(int32_t ctrl_id, nvme_namespace_t* out_ns, uint32_t max_ns) {
    if (ctrl_id < 0 || ctrl_id >= (int32_t)num_nvme_devices || !out_ns || max_ns == 0) {
        return -1;
    }

    nvme_device_t* ctrl = &nvme_devices[ctrl_id];
    if (!ctrl->active) {
        return -1;
    }

    uint32_t* ns_list = (uint32_t*)hal_alloc_dma_page();
    if (!ns_list) return -1;
    memset(ns_list, 0, 4096);

    uint64_t ns_list_phys = (uint64_t)hal_virt_to_phys(ns_list);

    // 1. Query Active Namespace List (CNS = 0x10) starting from NSID 0
    nvme_sqe_t cmd = {0};
    cmd.opcode = NVME_ADMIN_IDENTIFY;
    cmd.nsid   = 0;
    cmd.prp1   = ns_list_phys;
    cmd.cdw10  = 0x10;

    bool list_ok = nvme_submit_admin_cmd(ctrl, &cmd, NULL);

    uint8_t* ns_id_buf = (uint8_t*)hal_alloc_dma_page();
    if (!ns_id_buf) {
        hal_free_dma_page(ns_list);
        return -1;
    }
    uint64_t ns_id_phys = (uint64_t)hal_virt_to_phys(ns_id_buf);

    int32_t ns_found = 0;

    if (!list_ok || ns_list[0] == 0) {
        ns_list[0] = 1;
    }

    for (uint32_t i = 0; i < 1024 && ns_found < (int32_t)max_ns; i++) {
        uint32_t nsid = ns_list[i];
        if (nsid == 0) break;

        memset(ns_id_buf, 0, 4096);
        nvme_sqe_t ns_cmd = {0};
        ns_cmd.opcode = NVME_ADMIN_IDENTIFY;
        ns_cmd.nsid   = nsid;
        ns_cmd.prp1   = ns_id_phys;
        ns_cmd.cdw10  = 0x00;

        if (nvme_submit_admin_cmd(ctrl, &ns_cmd, NULL)) {
            // FIX: Use proper struct layout rather than raw byte offsets
            nvme_identify_ns_t* id_ns = (nvme_identify_ns_t*)ns_id_buf;

            uint8_t flbas = id_ns->flbas & 0x0F;
            uint8_t lbads = id_ns->lbaf[flbas].lbads;

            // Fallback for valid NVMe block size range (minimum 512 bytes = 2^9)
            if (lbads < 9) {
                lbads = 9;
            }

            out_ns[ns_found].nsid         = nsid;
            out_ns[ns_found].sector_count = id_ns->nsze;
            out_ns[ns_found].sector_size  = (1U << lbads);

            ns_found++;
        }
    }

    hal_free_dma_page(ns_id_buf);
    hal_free_dma_page(ns_list);

    return ns_found;
}
/**
 * Retrieve the sector (block) size in bytes for a specific NVMe controller namespace.
 *
 * @param ctrl_id  Device ID returned by nvme_init()
 * @param nsid     Namespace ID (typically 1)
 * @return uint32_t Sector size in bytes (e.g., 512, 4096), or 0 on failure
 */
uint32_t nvme_get_sector_size(int32_t ctrl_id, uint32_t nsid) {
    if (ctrl_id < 0 || ctrl_id >= (int32_t)num_nvme_devices) {
        return 0;
    }

    nvme_device_t* ctrl = &nvme_devices[ctrl_id];
    if (!ctrl->active) {
        return 0;
    }

    // NSIDs are 1-based. If 0 is passed, default to namespace 1 or ctrl->nsid
    if (nsid == 0) {
        nsid = (ctrl->nsid > 0) ? ctrl->nsid : 1;
    }

    // Fast path: return stored size if querying primary namespace
    if (nsid == ctrl->nsid && ctrl->sector_size > 0) {
        return ctrl->sector_size;
    }

    // Allocate DMA buffer to inspect target namespace structure
    uint8_t* dma_buf = (uint8_t*)hal_alloc_dma_page();
    if (!dma_buf) return 0;

    uint64_t phys_buf = (uint64_t)hal_virt_to_phys(dma_buf);

    nvme_sqe_t cmd = {0};
    cmd.opcode = NVME_ADMIN_IDENTIFY; // 0x06
    cmd.nsid   = nsid;                // Guaranteed >= 1 now
    cmd.prp1   = phys_buf;
    cmd.cdw10  = 0x00;                // CNS = 00h (Identify Namespace)

    if (!nvme_submit_admin_cmd(ctrl, &cmd, NULL)) {
        hal_free_dma_page(dma_buf);
        return 0;
    }

    nvme_identify_ns_t* id_ns = (nvme_identify_ns_t*)dma_buf;

    // Extract active LBA format details
    uint8_t active_lbaf = id_ns->flbas & 0x0F;
    uint8_t lbads = id_ns->lbaf[active_lbaf].lbads;

    // Fallback if field is invalid
    if (lbads < 9) {
        lbads = 9; // Default to 2^9 = 512 bytes
    }

    uint32_t sector_size = 1U << lbads;

    hal_free_dma_page(dma_buf);
    return sector_size;
}