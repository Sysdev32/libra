#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <hals/xhci.h>
// Technoblade never dies
/* ============================================================================
 * xHCI CONSTANTS & REGISTER BITS
 * ============================================================================ */

#define XHCI_PCI_CLASS          0x0C
#define XHCI_PCI_SUBCLASS       0x03
#define XHCI_PCI_PROGIF         0x30

#define PAGE_SIZE               4096
#define HHDM_OFFSET             0xffff800000000000ULL

/* Page Table Flags */
#define PTE_PRESENT             (1ULL << 0)
#define PTE_WRITABLE            (1ULL << 1)
#define PTE_WRITE_THROUGH       (1ULL << 3)
#define PTE_CACHE_DISABLE       (1ULL << 4)

/* USBCMD Bits */
#define CMD_RUN                 (1 << 0)
#define CMD_RESET               (1 << 1)
#define CMD_INTE                (1 << 2)

/* USBSTS Bits */
#define STS_HALTED              (1 << 0)
#define STS_FATAL               (1 << 2)
#define STS_CNR                 (1 << 11)

/* PORTSC Bits */
#define PORT_CCS                (1 << 0)   /* Current Connect Status */
#define PORT_PED                (1 << 1)   /* Port Enabled/Disabled */
#define PORT_PR                 (1 << 4)   /* Port Reset */
#define PORT_PLS_MASK           (0xF << 5)
#define PORT_PP                 (1 << 9)   /* Port Power */
#define PORT_SPEED_MASK         (0xF << 10)
#define PORT_CSC                (1 << 17)  /* Connect Status Change */

/* Ring Sizes */
#define CMD_RING_SIZE           64
#define EVENT_RING_SIZE         64
#define XFER_RING_SIZE          64

typedef uint64_t page_table_t;

extern page_table_t *vmm_get_current_pml4(void);
extern void  vmm_map_page(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
extern uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fun, uint8_t reg);
extern void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fun, uint8_t reg, uint32_t val);
extern uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fun, uint8_t reg);
extern void     pci_write16(uint8_t bus, uint8_t dev, uint8_t fun, uint8_t reg, uint16_t val);
#include <drivers/fb.h>
/* ============================================================================
 * DRIVER STATE & DMA POOL
 * ============================================================================ */

#define XHCI_DMA_POOL_SIZE (256 * 1024)

static xhci_controller_t g_xhci = {0};
static uint8_t g_xhci_dma_pool[XHCI_DMA_POOL_SIZE] __attribute__((aligned(4096)));
static size_t  g_xhci_dma_offset = 0;

static xhci_ring_t g_ep_rings[XHCI_MAX_DEVICES][32];
static usb_device_t g_usb_devices[XHCI_MAX_DEVICES] = {0};

/* ============================================================================
 * LOGGING UTILITIES
 * ============================================================================ */

static void xhci_log_trb(const char* context, xhci_trb_t* trb, uint32_t index) {
    uint8_t trb_type = (trb->control >> 10) & 0x3F;
    uint8_t cycle    = trb->control & TRB_CYCLE;
    printk(LOG_TRACE, "[xHCI] %s [TRB #%u] Type: %u, Cycle: %u, Flags: 0x%03X, Param: 0x%016llX, Status: 0x%08X\n",
           context, index, trb_type, cycle, trb->control & 0x3FF, (unsigned long long)trb->parameter, trb->status);
}

static void xhci_log_buffer_hex(const char* label, const void* buffer, size_t length) {
    
}

/* ============================================================================
 * VMM & DMA MEMORY ALLOCATOR
 * ============================================================================ */

static inline uintptr_t read_cr3(void) {
    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static uintptr_t hal_virt_to_phys(void* virt) {
    uintptr_t vaddr = (uintptr_t)virt;
    if (!vaddr) {
        printk(LOG_ERROR, "[xHCI] [V2P] NULL virtual address passed!\n");
        return 0;
    }

    uintptr_t pml4_phys = read_cr3() & ~0xFFFULL;
    if (!pml4_phys) return 0;

    page_table_t *pml4 = (page_table_t*)(pml4_phys + HHDM_OFFSET);

    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx   = (vaddr >> 21) & 0x1FF;
    uint64_t pt_idx   = (vaddr >> 12) & 0x1FF;
    uint64_t offset   = vaddr & 0xFFF;

    uint64_t pml4e = pml4[pml4_idx];
    if (!(pml4e & PTE_PRESENT)) return 0;

    page_table_t *pdpt = (page_table_t*)((pml4e & ~0xFFFULL & 0x000FFFFFFFFFF000ULL) + HHDM_OFFSET);
    uint64_t pdpte = pdpt[pdpt_idx];
    if (!(pdpte & PTE_PRESENT)) return 0;
    if (pdpte & (1 << 7)) return (pdpte & ~0x3FFFFFFFULL & 0x000FFFFFFFFFF000ULL) + (vaddr & 0x3FFFFFFFULL);

    page_table_t *pd = (page_table_t*)((pdpte & ~0xFFFULL & 0x000FFFFFFFFFF000ULL) + HHDM_OFFSET);
    uint64_t pde = pd[pd_idx];
    if (!(pde & PTE_PRESENT)) return 0;
    if (pde & (1 << 7)) return (pde & ~0x1FFFFFULL & 0x000FFFFFFFFFF000ULL) + (vaddr & 0x1FFFFFULL);

    page_table_t *pt = (page_table_t*)((pde & ~0xFFFULL & 0x000FFFFFFFFFF000ULL) + HHDM_OFFSET);
    uint64_t pte = pt[pt_idx];
    if (!(pte & PTE_PRESENT)) return 0;

    return (pte & ~0xFFFULL & 0x000FFFFFFFFFF000ULL) + offset;
}

static void* hal_alloc_dma_aligned(size_t size, size_t alignment) {
    size_t aligned_offset = (g_xhci_dma_offset + (alignment - 1)) & ~(alignment - 1);

    if (aligned_offset + size > XHCI_DMA_POOL_SIZE) {
        printk(LOG_ERROR, "[xHCI] [DMA Alloc] CRITICAL: Static DMA pool exhausted! Requested: %zu B\n", size);
        return NULL;
    }

    void* virt = &g_xhci_dma_pool[aligned_offset];
    g_xhci_dma_offset = aligned_offset + size;
    memset(virt, 0, size);

    return virt;
}

static void* hal_mmio_map(uintptr_t phys_addr, size_t size) {
    page_table_t *pml4 = vmm_get_current_pml4();
    uint64_t virt_start = phys_addr + HHDM_OFFSET;
    uint64_t phys_page  = phys_addr & ~(PAGE_SIZE - 1);
    uint64_t offset     = phys_addr & (PAGE_SIZE - 1);
    uint64_t num_pages  = ((uint64_t)size + offset + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t curr_phys = phys_page + (i * PAGE_SIZE);
        uint64_t curr_virt = (virt_start & ~(PAGE_SIZE - 1)) + (i * PAGE_SIZE);
        vmm_map_page(pml4, curr_virt, curr_phys, PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE | PTE_WRITE_THROUGH);
        asm volatile("invlpg (%0)" :: "r"(curr_virt) : "memory");
    }

    return (void*)(virt_start);
}

static void delay_ms(uint32_t ms) {
    for (volatile uint64_t i = 0; i < ms * 100000; i++) {
        asm volatile("pause");
    }
}

/* ============================================================================
 * RING MANAGEMENT ENGINE
 * ============================================================================ */

static void xhci_init_ring(xhci_ring_t* ring, uint32_t num_trbs) {
    ring->ring = (xhci_trb_t*)hal_alloc_dma_aligned(num_trbs * sizeof(xhci_trb_t), 64);
    ring->size = num_trbs;
    ring->enqueue_idx = 0;
    ring->dequeue_idx = 0;
    ring->cycle_state = 1;

    /* Setup Link TRB at ring tail */
    xhci_trb_t* link_trb = &ring->ring[num_trbs - 1];
    link_trb->parameter = hal_virt_to_phys(ring->ring);
    link_trb->status    = 0;
    link_trb->control   = (TRB_TYPE_LINK << 10) | (1 << 1); /* Toggle Cycle Bit */
}

static xhci_trb_t* xhci_enqueue_trb(xhci_ring_t* ring) {
    if (!ring || !ring->ring) {
        printk(LOG_ERROR, "[xHCI] Attempted enqueue on uninitialized ring!\n");
        return NULL;
    }

    xhci_trb_t* trb = &ring->ring[ring->enqueue_idx];

    ring->enqueue_idx++;
    if (ring->enqueue_idx == ring->size - 1) {
        xhci_trb_t* link_trb = &ring->ring[ring->enqueue_idx];
        if (ring->cycle_state) {
            link_trb->control |= TRB_CYCLE;
        } else {
            link_trb->control &= ~TRB_CYCLE;
        }
        ring->cycle_state ^= 1;
        ring->enqueue_idx = 0;
    }

    return trb;
}

static void xhci_ring_doorbell(uint8_t slot_id, uint8_t target) {
    g_xhci.doorbells[slot_id] = target;
}

/* ============================================================================
 * EVENT WAIT ENGINE
 * ============================================================================ */

static bool xhci_wait_for_event(uint8_t expected_type, xhci_trb_t* out_trb) {
    uint32_t timeout = 5000;
    xhci_ring_t* er = &g_xhci.event_ring;

    while (timeout--) {
        xhci_trb_t* trb = &er->ring[er->dequeue_idx];
        uint8_t cycle = trb->control & TRB_CYCLE;

        if (cycle == er->cycle_state) {
            uint8_t type = (trb->control >> 10) & 0x3F;
            if (out_trb) {
                *out_trb = *trb;
            }

            er->dequeue_idx++;
            if (er->dequeue_idx == er->size) {
                er->dequeue_idx = 0;
                er->cycle_state ^= 1;
            }

            /* Update ERDP */
            uint64_t erdp_phys = hal_virt_to_phys(&er->ring[er->dequeue_idx]);
            g_xhci.interrupter0->erdp = erdp_phys | (1 << 3); /* Clear EHB bit */

            if (type == expected_type) {
                return true;
            }
        }
        delay_ms(1);
    }

    printk(LOG_ERROR, "[xHCI] TIMEOUT waiting for Event TRB Type %u!\n", expected_type);
    return false;
}

/* ============================================================================
 * ENDPOINT CONFIGURATION ENGINE
 * ============================================================================ */

bool xhci_configure_endpoint(uint8_t slot_id, uint8_t ep_addr, uint16_t max_packet_size) {
    uint8_t ep_num = ep_addr & 0x0F;
    bool is_in     = (ep_addr & 0x80) != 0;
    uint8_t ep_idx = (ep_num * 2) + (is_in ? 1 : 0);

    printk(LOG_INFO, "[xHCI] Configuring Slot %u, EP 0x%02X (DCI %u, MaxPacket: %u)\n", 
           slot_id, ep_addr, ep_idx, max_packet_size);

    /* Allocate Transfer Ring for Endpoint */
    xhci_init_ring(&g_ep_rings[slot_id][ep_idx], XFER_RING_SIZE);

    /* Allocate Input Context */
    uint8_t* input_ctx = (uint8_t*)hal_alloc_dma_aligned(PAGE_SIZE, 64);
    xhci_input_control_context_t* ctrl_ctx = (xhci_input_control_context_t*)input_ctx;
    xhci_slot_context_t*          slot_ctx = (xhci_slot_context_t*)(input_ctx + 32);
    xhci_ep_context_t*            ep_ctx   = (xhci_ep_context_t*)(input_ctx + 32 + (ep_idx * 32));

    ctrl_ctx->add_flags = (1 << 0) | (1 << ep_idx);
    slot_ctx->info1 = (ep_idx << 27); /* Set context entry limit */

    uint8_t ep_type = is_in ? 7 : 3; /* Interrupt IN (7) or Interrupt OUT (3) */
    ep_ctx->ep_info2 = (ep_type << 3) | (max_packet_size << 16);
    ep_ctx->tr_dequeue_ptr = hal_virt_to_phys(g_ep_rings[slot_id][ep_idx].ring) | g_ep_rings[slot_id][ep_idx].cycle_state;
    ep_ctx->avg_trb_length = max_packet_size;
    ep_ctx->interval = 6; /* 4ms polling interval */

    /* Issue Configure Endpoint Command */
    xhci_trb_t* cmd = xhci_enqueue_trb(&g_xhci.cmd_ring);
    cmd->parameter = hal_virt_to_phys(input_ctx);
    cmd->status    = 0;
    cmd->control   = (TRB_TYPE_CONFIGURE_ENDPOINT << 10) | (slot_id << 24) | (g_xhci.cmd_ring.cycle_state ? TRB_CYCLE : 0);

    xhci_ring_doorbell(0, 0);

    xhci_trb_t event;
    return xhci_wait_for_event(TRB_TYPE_CMD_COMPLETION, &event);
}

/* ============================================================================
 * CONTROL & BULK / INTERRUPT TRANSFERS
 * ============================================================================ */

bool xhci_control_transfer(uint8_t slot_id, usb_setup_packet_t* setup, void* data_buf) {
    if (!g_xhci.active || slot_id == 0) return false;

    xhci_ring_t* ep0_ring = &g_ep_rings[slot_id][1];

    /* 1. Setup Stage TRB */
    xhci_trb_t* setup_trb = xhci_enqueue_trb(ep0_ring);
    setup_trb->parameter = *(uint64_t*)setup;
    setup_trb->status    = 8;
    setup_trb->control   = (TRB_TYPE_SETUP_STAGE << 10) | (ep0_ring->cycle_state ? TRB_CYCLE : 0) | TRB_IDT;

    uint8_t trt = 0;
    if (setup->length > 0) {
        trt = (setup->request_type & 0x80) ? 3 : 2;
    }
    setup_trb->control |= (trt << 16);

    /* 2. Data Stage TRB (Optional) */
    if (setup->length > 0 && data_buf != NULL) {
        xhci_trb_t* data_trb = xhci_enqueue_trb(ep0_ring);
        data_trb->parameter = hal_virt_to_phys(data_buf);
        data_trb->status    = setup->length;
        data_trb->control   = (TRB_TYPE_DATA_STAGE << 10) | (ep0_ring->cycle_state ? TRB_CYCLE : 0);
        if (setup->request_type & 0x80) {
            data_trb->control |= (1 << 16); /* IN */
        }
    }

    /* 3. Status Stage TRB */
    xhci_trb_t* status_trb = xhci_enqueue_trb(ep0_ring);
    status_trb->parameter = 0;
    status_trb->status    = 0;
    status_trb->control   = (TRB_TYPE_STATUS_STAGE << 10) | (ep0_ring->cycle_state ? TRB_CYCLE : 0) | TRB_IOC;
    if (setup->length > 0 && !(setup->request_type & 0x80)) {
        status_trb->control |= (1 << 16);
    }

    xhci_ring_doorbell(slot_id, 1);

    xhci_trb_t event;
    bool success = xhci_wait_for_event(TRB_TYPE_TRANSFER_EVENT, &event);

    if (success && setup->length > 0 && data_buf != NULL) {
        xhci_log_buffer_hex("Control Transfer Data", data_buf, setup->length);
    }

    return success;
}

bool xhci_transfer_io(uint8_t slot_id, uint8_t ep_addr, void* buffer, uint32_t length) {
    uint8_t ep_num = ep_addr & 0x0F;
    bool is_in     = (ep_addr & 0x80) != 0;
    uint8_t ep_idx = (ep_num * 2) + (is_in ? 1 : 0);

    if (!g_xhci.active || slot_id == 0 || length == 0) return false;

    xhci_ring_t* ring = &g_ep_rings[slot_id][ep_idx];
    if (!ring->ring) {
        printk(LOG_ERROR, "[xHCI] IO Transfer failed: EP ring %u on Slot %u not initialized!\n", ep_idx, slot_id);
        return false;
    }

    xhci_trb_t* trb = xhci_enqueue_trb(ring);
    trb->parameter = hal_virt_to_phys(buffer);
    trb->status    = length;
    trb->control   = (TRB_TYPE_NORMAL << 10) | (ring->cycle_state ? TRB_CYCLE : 0) | TRB_IOC;

    xhci_ring_doorbell(slot_id, ep_idx);

    xhci_trb_t event;
    bool success = xhci_wait_for_event(TRB_TYPE_TRANSFER_EVENT, &event);

    if (success && is_in) {
        xhci_log_buffer_hex("IO Read Data", buffer, length);
    }

    return success;
}

bool xhci_bulk_read(uint8_t slot_id, uint8_t ep_in, void* buffer, uint32_t length) {
    return xhci_transfer_io(slot_id, ep_in | 0x80, buffer, length);
}

bool xhci_bulk_write(uint8_t slot_id, uint8_t ep_out, const void* buffer, uint32_t length) {
    return xhci_transfer_io(slot_id, ep_out & 0x7F, (void*)buffer, length);
}

bool xhci_interrupt_transfer(uint8_t slot_id, uint8_t ep_in, void* buffer, uint32_t length) {
    return xhci_transfer_io(slot_id, ep_in | 0x80, buffer, length);
}

/* ============================================================================
 * DEVICE ENUMERATION ENGINE
 * ============================================================================ */

static bool xhci_enable_slot(uint8_t* slot_id_out) {
    xhci_trb_t* cmd = xhci_enqueue_trb(&g_xhci.cmd_ring);
    cmd->parameter = 0;
    cmd->status    = 0;
    cmd->control   = (TRB_TYPE_ENABLE_SLOT << 10) | (g_xhci.cmd_ring.cycle_state ? TRB_CYCLE : 0);

    xhci_ring_doorbell(0, 0);

    xhci_trb_t event;
    if (xhci_wait_for_event(TRB_TYPE_CMD_COMPLETION, &event)) {
        *slot_id_out = (event.control >> 24) & 0xFF;
        return (*slot_id_out != 0);
    }

    return false;
}

bool xhci_enumerate_device(uint8_t port_idx, uint8_t speed) {
    printk(LOG_INFO, "[xHCI] Enumerating device on Root Hub Port %u (Speed: %u)\n", port_idx + 1, speed);

    uint8_t slot_id = 0;
    if (!xhci_enable_slot(&slot_id)) {
        printk(LOG_ERROR, "[xHCI] Failed to enable slot for Port %d\n", port_idx + 1);
        return false;
    }

    uint8_t* input_ctx = (uint8_t*)hal_alloc_dma_aligned(PAGE_SIZE, 64);
    uint8_t* dev_ctx   = (uint8_t*)hal_alloc_dma_aligned(PAGE_SIZE, 64);

    g_xhci.dcbaa[slot_id] = hal_virt_to_phys(dev_ctx);

    xhci_input_control_context_t* ctrl_ctx = (xhci_input_control_context_t*)input_ctx;
    xhci_slot_context_t*          slot_ctx = (xhci_slot_context_t*)(input_ctx + 32);
    xhci_ep_context_t*            ep0_ctx  = (xhci_ep_context_t*)(input_ctx + 64);

    ctrl_ctx->add_flags = (1 << 0) | (1 << 1);

    slot_ctx->info1 = (1 << 27) | (speed << 20);
    slot_ctx->info2 = (port_idx + 1) << 16;

    xhci_init_ring(&g_ep_rings[slot_id][1], XFER_RING_SIZE);

    ep0_ctx->ep_info2 = (4 << 3) | (64 << 16);
    ep0_ctx->tr_dequeue_ptr = hal_virt_to_phys(g_ep_rings[slot_id][1].ring) | g_ep_rings[slot_id][1].cycle_state;
    ep0_ctx->avg_trb_length = 8;

    xhci_trb_t* cmd = xhci_enqueue_trb(&g_xhci.cmd_ring);
    cmd->parameter = hal_virt_to_phys(input_ctx);
    cmd->status    = 0;
    cmd->control   = (TRB_TYPE_ADDRESS_DEVICE << 10) | (slot_id << 24) | (g_xhci.cmd_ring.cycle_state ? TRB_CYCLE : 0);

    xhci_ring_doorbell(0, 0);

    xhci_trb_t event;
    if (!xhci_wait_for_event(TRB_TYPE_CMD_COMPLETION, &event)) {
        printk(LOG_ERROR, "[xHCI] ADDRESS_DEVICE command failed for Slot %u!\n", slot_id);
        return false;
    }

    usb_device_t* dev = &g_usb_devices[slot_id];
    dev->address = slot_id;
    dev->speed   = speed;
    dev->connected = true;

    /* Fetch Device Descriptor */
    usb_setup_packet_t setup;
    usb_device_descriptor_t* dev_desc = (usb_device_descriptor_t*)hal_alloc_dma_aligned(64, 32);

    setup.request_type = 0x80;
    setup.request      = 0x06;
    setup.value        = (0x01 << 8);
    setup.index        = 0;
    setup.length       = 18;

    if (xhci_control_transfer(slot_id, &setup, dev_desc)) {
        printk(LOG_INFO, "[xHCI] Device Registered [Slot %u]: Vendor=0x%04X, Product=0x%04X, Class=0x%02X\n",
               slot_id, dev_desc->idVendor, dev_desc->idProduct, dev_desc->bDeviceClass);
    }

    return true;
}

/* ============================================================================
 * PORT SCANNING ENGINE
 * ============================================================================ */

void xhci_scan_ports(void) {
    if (!g_xhci.active) return;

    printk(LOG_INFO, "[xHCI] Scanning %d root hub ports...\n", g_xhci.max_ports);

    for (uint8_t i = 0; i < g_xhci.max_ports; i++) {
        volatile uint32_t* portsc_ptr = (volatile uint32_t*)((uintptr_t)g_xhci.op_regs + 0x400 + (i * 0x10));
        uint32_t portsc = *portsc_ptr;

        /* Ensure Port Power is enabled */
        if (!(portsc & PORT_PP)) {
            *portsc_ptr = (portsc & ~0x00FE0000) | PORT_PP;
            delay_ms(20);
            portsc = *portsc_ptr;
        }

        if (portsc & PORT_CCS) {
            uint8_t speed = (portsc & PORT_SPEED_MASK) >> 10;
            printk(LOG_INFO, "[xHCI] Port %u: Device detected (Speed: %u). Resetting...\n", i + 1, speed);

            *portsc_ptr = (portsc & ~0x00FE0000) | PORT_PR;
            delay_ms(50);

            portsc = *portsc_ptr;
            xhci_enumerate_device(i, speed);
        } else {
            printk(LOG_TRACE, "[xHCI] Port %u is empty.\n", i + 1);
        }
    }
}

int xhci_find_devices_by_class(uint8_t target_class, usb_matched_device_t* out_devices, int max_results) {
    if (!g_xhci.active || !out_devices || max_results <= 0) return 0;

    int match_count = 0;

    for (uint8_t slot = 1; slot < g_xhci.max_slots; slot++) {
        usb_device_t* dev = &g_usb_devices[slot];
        if (!dev->connected) continue;

        usb_setup_packet_t setup;
        uint8_t* cfg_buf = (uint8_t*)hal_alloc_dma_aligned(512, 32);
        if (!cfg_buf) continue;

        setup.request_type = 0x80;
        setup.request      = 0x06;
        setup.value        = (0x02 << 8);
        setup.index        = 0;
        setup.length       = 512;

        if (xhci_control_transfer(slot, &setup, cfg_buf)) {
            usb_config_descriptor_t* cfg = (usb_config_descriptor_t*)cfg_buf;
            uint8_t* ptr = cfg_buf + cfg->bLength;

            while (ptr < cfg_buf + cfg->wTotalLength) {
                uint8_t desc_len  = ptr[0];
                uint8_t desc_type = ptr[1];

                if (desc_len == 0) break;

                if (desc_type == 0x04) { /* Interface Descriptor */
                    usb_interface_descriptor_t* iface = (usb_interface_descriptor_t*)ptr;

                    if (iface->bInterfaceClass == target_class || target_class == 0xFF) {
                        out_devices[match_count].port            = slot;
                        out_devices[match_count].address         = slot;
                        out_devices[match_count].device_class    = iface->bInterfaceClass;
                        out_devices[match_count].device_subclass = iface->bInterfaceSubClass;
                        out_devices[match_count].device_protocol = iface->bInterfaceProtocol;

                        match_count++;
                        break;
                    }
                }
                ptr += desc_len;
            }
        }

        if (match_count >= max_results) break;
    }

    return match_count;
}

/* ============================================================================
 * PCI PROBING & DRIVER INIT
 * ============================================================================ */

static void xhci_bios_handoff(uint8_t bus, uint8_t dev, uint8_t fun) {
    uint32_t hccparams1 = g_xhci.cap_regs->hccparams1;
    uint32_t xecp = (hccparams1 >> 16) & 0xFFFF;

    if (!xecp) return;

    uint32_t legsup_addr = (xecp << 2);
    uint32_t legsup = pci_read32(bus, dev, fun, legsup_addr);

    if (legsup & (1 << 16)) {
        printk(LOG_INFO, "[xHCI] BIOS ownership detected. Requesting handoff...\n");
        pci_write32(bus, dev, fun, legsup_addr, legsup | (1 << 24));

        uint32_t timeout = 100;
        while ((pci_read32(bus, dev, fun, legsup_addr) & (1 << 16)) && --timeout) {
            delay_ms(10);
        }
    }
}

static bool xhci_reset(void) {
    g_xhci.op_regs->usbcmd &= ~CMD_RUN;

    uint32_t timeout = 100;
    while (!(g_xhci.op_regs->usbsts & STS_HALTED) && --timeout) {
        delay_ms(1);
    }

    g_xhci.op_regs->usbcmd |= CMD_RESET;

    timeout = 100;
    while ((g_xhci.op_regs->usbcmd & CMD_RESET) && --timeout) {
        delay_ms(1);
    }

    timeout = 100;
    while ((g_xhci.op_regs->usbsts & STS_CNR) && --timeout) {
        delay_ms(1);
    }

    return !(g_xhci.op_regs->usbsts & STS_CNR);
}

bool xhci_init_device(uint8_t bus, uint8_t dev, uint8_t fun) {
    uint16_t cmd = pci_read16(bus, dev, fun, 0x04);
    pci_write16(bus, dev, fun, 0x04, cmd | (1 << 1) | (1 << 2));

    uint32_t bar0 = pci_read32(bus, dev, fun, 0x10);
    uint32_t bar1 = pci_read32(bus, dev, fun, 0x14);
    uintptr_t phys_mmio = (bar0 & 0xFFFFFFF0) | ((uint64_t)bar1 << 32);

    /* FIX: Map full 64KB region to cover CAP, OP, RTS, and Doorbell registers */
    g_xhci.cap_regs   = (xhci_cap_regs_t*)hal_mmio_map(phys_mmio, 0x10000);
    g_xhci.op_regs    = (xhci_op_regs_t*)((uintptr_t)g_xhci.cap_regs + g_xhci.cap_regs->caplength);
    g_xhci.doorbells  = (volatile uint32_t*)((uintptr_t)g_xhci.cap_regs + g_xhci.cap_regs->dboff);
    g_xhci.interrupter0 = (xhci_interrupter_t*)((uintptr_t)g_xhci.cap_regs + g_xhci.cap_regs->rtsoff + 0x20);

    g_xhci.max_slots = g_xhci.cap_regs->hcsparams1 & 0xFF;
    g_xhci.max_ports = (g_xhci.cap_regs->hcsparams1 >> 24) & 0xFF;

    printk(LOG_INFO, "[xHCI] Controller found at B%d:D%d:F%d (Slots: %u, Ports: %u)\n", 
           bus, dev, fun, g_xhci.max_slots, g_xhci.max_ports);

    xhci_bios_handoff(bus, dev, fun);

    if (!xhci_reset()) {
        printk(LOG_ERROR, "[xHCI] Controller reset failed!\n");
        return false;
    }

    /* DCBAA Initialization */
    g_xhci.dcbaa = (uint64_t*)hal_alloc_dma_aligned((g_xhci.max_slots + 1) * sizeof(uint64_t), 64);
    g_xhci.op_regs->dcbaap = hal_virt_to_phys(g_xhci.dcbaa);

    /* Command Ring Initialization */
    xhci_init_ring(&g_xhci.cmd_ring, CMD_RING_SIZE);
    g_xhci.op_regs->crcr = hal_virt_to_phys(g_xhci.cmd_ring.ring) | g_xhci.cmd_ring.cycle_state;

    /* Event Ring Initialization */
    xhci_init_ring(&g_xhci.event_ring, EVENT_RING_SIZE);
    g_xhci.erst = (xhci_erst_entry_t*)hal_alloc_dma_aligned(sizeof(xhci_erst_entry_t), 64);
    g_xhci.erst->ring_segment_base_address = hal_virt_to_phys(g_xhci.event_ring.ring);
    g_xhci.erst->ring_segment_size         = EVENT_RING_SIZE;

    g_xhci.interrupter0->erstsz = 1;
    g_xhci.interrupter0->erdp   = hal_virt_to_phys(g_xhci.event_ring.ring);
    g_xhci.interrupter0->erstba = hal_virt_to_phys(g_xhci.erst);

    /* Enable Slots & Run */
    g_xhci.op_regs->config = g_xhci.max_slots;
    g_xhci.op_regs->usbcmd |= CMD_RUN;
    g_xhci.active = true;

    xhci_scan_ports();
    return true;
}

void xhci_init(void) {
    printk(LOG_INFO, "[xHCI] Probing PCI bus for xHCI controllers...\n");

    for (uint16_t b = 0; b < 256; b++) {
        for (uint8_t d = 0; d < 32; d++) {
            for (uint8_t f = 0; f < 8; f++) {
                uint32_t class_code = pci_read32(b, d, f, 0x08);
                uint8_t base_class = (class_code >> 24) & 0xFF;
                uint8_t sub_class  = (class_code >> 16) & 0xFF;
                uint8_t prog_if    = (class_code >> 8)  & 0xFF;

                if (base_class == XHCI_PCI_CLASS &&
                    sub_class  == XHCI_PCI_SUBCLASS &&
                    prog_if    == XHCI_PCI_PROGIF) {

                    if (xhci_init_device(b, d, f)) return;
                }
            }
        }
    }

    printk(LOG_WARNING, "[xHCI] No active controllers detected.\n");
}