#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <hals/ehci.h>

/* ============================================================================
 * EHCI CONSTANTS & PCI DEFINITIONS
 * ============================================================================ */

#define EHCI_PCI_CLASS        0x0C
#define EHCI_PCI_SUBCLASS     0x03
#define EHCI_PCI_PROGIF       0x20

#define PAGE_SIZE             4096
#define HHDM_OFFSET           0xffff800000000000ULL

/* Page Table Entry Flags */
#define PTE_PRESENT          (1ULL << 0)
#define PTE_WRITABLE         (1ULL << 1)
#define PTE_WRITE_THROUGH    (1ULL << 3)
#define PTE_CACHE_DISABLE    (1ULL << 4)

/* USBCMD Register Bits */
#define CMD_RUN               (1 << 0)
#define CMD_RESET             (1 << 1)
#define CMD_PERIODIC_ENABLE    (1 << 4)
#define CMD_ASYNC_ENABLE      (1 << 5)

/* USBSTS Register Bits */
#define STS_HALTED            (1 << 12)
#define STS_ASYNC_ADVANCE     (1 << 5)
#define STS_HOST_ERROR        (1 << 4)
#define STS_PCI_ERROR         (1 << 3)
#define STS_PORT_CHANGE       (1 << 2)
#define STS_ERROR_INT         (1 << 1)
#define STS_INT               (1 << 0)

/* PORTSC Register Bits */
#define PORT_CONNECTION       (1 << 0)
#define PORT_CONNECT_CHANGE   (1 << 1)
#define PORT_ENABLE           (1 << 2)
#define PORT_RESET            (1 << 8)
#define PORT_POWER            (1 << 12)
#define PORT_OWNER            (1 << 13)

/* USB Speeds */
#define USB_SPEED_FULL        0x00
#define USB_SPEED_LOW         0x01
#define USB_SPEED_HIGH        0x02

/* Queue Descriptor Types & Terminate Bits */
#define QTD_PTR_INVALID       0x00000001
#define QH_PTR_TYPE_QH        0x00000002

/* USB PIDs */
#define USB_PID_OUT           0
#define USB_PID_IN            1
#define USB_PID_SETUP         2

/* Standard USB Request Types & Descriptors */
#define USB_REQ_GET_STATUS    0x00
#define USB_REQ_CLEAR_FEATURE 0x01
#define USB_REQ_SET_FEATURE   0x03
#define USB_REQ_SET_ADDRESS   0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_DESCRIPTOR 0x07
#define USB_REQ_GET_CONFIG    0x08
#define USB_REQ_SET_CONFIG    0x09
#define USB_REQ_GET_INTERFACE 0x0A
#define USB_REQ_SET_INTERFACE 0x0B

#define USB_DESC_DEVICE       0x01
#define USB_DESC_CONFIG       0x02
#define USB_DESC_STRING       0x03
#define USB_DESC_INTERFACE    0x04
#define USB_DESC_ENDPOINT     0x05
#define USB_DESC_HID          0x21
#define USB_DESC_REPORT       0x22
#define USB_DESC_PHYSICAL     0x23

/* Endpoint Attributes */
#define EP_TYPE_CONTROL       0x00
#define EP_TYPE_ISOCHRONOUS   0x01
#define EP_TYPE_BULK          0x02
#define EP_TYPE_INTERRUPT     0x03

/* Kernel Logging Levels */
#define LOG_TRACE   0
#define LOG_INFO    1
#define LOG_WARNING 2
#define LOG_ERROR   3

/* External Kernel Symbols */
typedef uint64_t page_table_t;

extern page_table_t *vmm_get_current_pml4(void);
extern void  vmm_map_page(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
extern uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fun, uint8_t reg);
extern void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fun, uint8_t reg, uint32_t val);
extern uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fun, uint8_t reg);
extern void     pci_write16(uint8_t bus, uint8_t dev, uint8_t fun, uint8_t reg, uint16_t val);
extern void     printk(int level, const char* fmt, ...);

/* ============================================================================
 * ALL STRUCTS & HARDWARE STRUCTURES
 * ============================================================================ */

static ehci_controller_t g_ehci = {0};
static ehci_usb_device_t     g_usb_devices[EHCI_MAX_DEVICES] = {0};

/* ============================================================================
 * STATIC BSS DMA POOL
 * ============================================================================ */

#define EHCI_DMA_POOL_SIZE (128 * 1024)

static uint8_t g_ehci_dma_pool[EHCI_DMA_POOL_SIZE] __attribute__((aligned(4096)));
static size_t  g_ehci_dma_offset = 0;

/* ============================================================================
 * PAGE TABLE WALKER & VMM MAPPER
 * ============================================================================ */

static inline uintptr_t read_cr3(void) {
    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

uintptr_t hal_virt_to_phys(void* virt) {
    uintptr_t vaddr = (uintptr_t)virt;
    if (!vaddr) return 0;

    uintptr_t pml4_phys = read_cr3() & ~0xFFFULL;
    if (!pml4_phys) {
        printk(LOG_ERROR, "EHCI [HAL]: CR3 contains NULL PML4 address!\n");
        return 0;
    }

    page_table_t *pml4 = (page_table_t*)(pml4_phys + HHDM_OFFSET);

    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx   = (vaddr >> 21) & 0x1FF;
    uint64_t pt_idx   = (vaddr >> 12) & 0x1FF;
    uint64_t offset   = vaddr & 0xFFF;

    uint64_t pml4e = pml4[pml4_idx];
    if (!(pml4e & PTE_PRESENT)) return 0;

    uintptr_t pdpt_phys = pml4e & ~0xFFFULL & 0x000FFFFFFFFFF000ULL;
    page_table_t *pdpt = (page_table_t*)(pdpt_phys + HHDM_OFFSET);
    uint64_t pdpte = pdpt[pdpt_idx];

    if (!(pdpte & PTE_PRESENT)) return 0;

    if (pdpte & (1 << 7)) {
        uint64_t phys_page = pdpte & ~0x3FFFFFFFULL & 0x000FFFFFFFFFF000ULL;
        return phys_page + (vaddr & 0x3FFFFFFFULL);
    }

    uintptr_t pd_phys = pdpte & ~0xFFFULL & 0x000FFFFFFFFFF000ULL;
    page_table_t *pd = (page_table_t*)(pd_phys + HHDM_OFFSET);
    uint64_t pde = pd[pd_idx];

    if (!(pde & PTE_PRESENT)) return 0;

    if (pde & (1 << 7)) {
        uint64_t phys_page = pde & ~0x1FFFFFULL & 0x000FFFFFFFFFF000ULL;
        return phys_page + (vaddr & 0x1FFFFFULL);
    }

    uintptr_t pt_phys = pde & ~0xFFFULL & 0x000FFFFFFFFFF000ULL;
    page_table_t *pt = (page_table_t*)(pt_phys + HHDM_OFFSET);
    uint64_t pte = pt[pt_idx];

    if (!(pte & PTE_PRESENT)) return 0;

    uint64_t phys_page = pte & ~0xFFFULL & 0x000FFFFFFFFFF000ULL;
    return phys_page + offset;
}

static void* hal_alloc_dma_aligned(size_t size, size_t alignment) {
    size_t aligned_offset = (g_ehci_dma_offset + (alignment - 1)) & ~(alignment - 1);

    if (aligned_offset + size > EHCI_DMA_POOL_SIZE) {
        printk(LOG_ERROR, "EHCI [HAL]: Static DMA pool exhausted allocating %u bytes!\n", (uint32_t)size);
        return NULL;
    }

    void* virt = &g_ehci_dma_pool[aligned_offset];
    g_ehci_dma_offset = aligned_offset + size;
    memset(virt, 0, size);

    uintptr_t phys = hal_virt_to_phys(virt);
    if (phys > 0xFFFFFFFFULL) {
        printk(LOG_ERROR, "EHCI [HAL]: FATAL - Physical address exceeds 32-bit limit!\n");
        return NULL;
    }

    return virt;
}

static void hal_free_dma_aligned(void* ptr) {
    (void)ptr;
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
        uint64_t flags = PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE | PTE_WRITE_THROUGH;
        vmm_map_page(pml4, curr_virt, curr_phys, flags);
        asm volatile("invlpg (%0)" :: "r"(curr_virt) : "memory");
    }

    return (void *)(virt_start);
}

static void delay_ms(uint32_t ms) {
    for (volatile uint64_t i = 0; i < ms * 100000; i++) {
        asm volatile("pause");
    }
}

/* ============================================================================
 * BIOS-TO-OS HANDOFF & HARDWARE RESET
 * ============================================================================ */

static void ehci_bios_handoff(uint8_t bus, uint8_t dev, uint8_t fun, uint32_t hccparams) {
    uint8_t eecp = (hccparams >> 8) & 0xFF;
    if (eecp < 0x40) return;

    uint32_t legsup = pci_read32(bus, dev, fun, eecp);
    if (legsup & (1 << 16)) {
        printk(LOG_WARNING, "EHCI [Handoff]: BIOS-owned! Requesting ownership...\n");
        pci_write32(bus, dev, fun, eecp, legsup | (1 << 24));

        uint32_t timeout = 100;
        while ((pci_read32(bus, dev, fun, eecp) & (1 << 16)) && --timeout) {
            delay_ms(10);
        }
    }
}

static bool ehci_reset(ehci_controller_t* ehci) {
    ehci->op_regs->usbcmd &= ~CMD_RUN;

    uint32_t timeout = 100;
    while (!(ehci->op_regs->usbsts & STS_HALTED) && --timeout) {
        delay_ms(1);
    }

    if (!timeout) return false;

    ehci->op_regs->usbcmd |= CMD_RESET;
    timeout = 100;
    while ((ehci->op_regs->usbcmd & CMD_RESET) && --timeout) {
        delay_ms(1);
    }

    return (timeout > 0);
}

/* ============================================================================
 * QUEUE HEAD SPLIT-TRANSACTION BUILDER ENGINE
 * ============================================================================ */

static void ehci_init_qh_for_ep(ehci_qh_t* qh, ehci_usb_device_t* dev, uint8_t ep_num, uint16_t max_packet, uint8_t ep_type) {
    uint32_t eps_speed = 0;
    if (dev->speed == USB_SPEED_LOW)       eps_speed = (1 << 12);
    else if (dev->speed == USB_SPEED_HIGH) eps_speed = (2 << 12);
    else                                  eps_speed = (0 << 12); /* Full-Speed */

    qh->characteristics = (dev->address & 0x7F)
                        | ((ep_num & 0x0F) << 8)
                        | eps_speed
                        | ((uint32_t)(max_packet & 0x7FF) << 16)
                        | (1 << 14); /* Data Toggle Control */

    if (ep_num == 0 && dev->speed != USB_SPEED_HIGH) {
        qh->characteristics |= (1 << 27); /* Control Endpoint Flag */
    }

    if (dev->address == 0 && ep_num == 0 && dev->speed == USB_SPEED_HIGH) {
        qh->characteristics |= (1 << 15); /* Head of Reclamation List */
    }

    qh->capabilities = 0;

    if (dev->speed != USB_SPEED_HIGH && dev->is_behind_hub) {
        qh->capabilities |= ((uint32_t)(dev->hub_address & 0x7F) << 16);
        qh->capabilities |= ((uint32_t)(dev->hub_port_num & 0x7F) << 23);

        if (ep_type == EP_TYPE_INTERRUPT) {
            qh->capabilities |= (1 << 0);       /* Start-Split Mask */
            qh->capabilities |= (0x1C << 8);    /* Complete-Split Mask */
        }
    }

    qh->capabilities |= (1 << 30); /* Mult = 1 */
}

/* ============================================================================
 * SCHEDULE SETUP
 * ============================================================================ */

static void ehci_setup_schedules(ehci_controller_t* ehci) {
    ehci->periodic_list = (uint32_t*)hal_alloc_dma_aligned(1024 * sizeof(uint32_t), 4096);
    ehci->interrupt_qh  = (ehci_qh_t*)hal_alloc_dma_aligned(sizeof(ehci_qh_t), 64);

    uint32_t intr_phys = (uint32_t)hal_virt_to_phys(ehci->interrupt_qh);

    ehci->interrupt_qh->horizontal_link = QTD_PTR_INVALID;
    ehci->interrupt_qh->characteristics = (1 << 15);
    ehci->interrupt_qh->capabilities    = (1 << 30);
    ehci->interrupt_qh->overlay.next     = QTD_PTR_INVALID;
    ehci->interrupt_qh->overlay.alt_next = QTD_PTR_INVALID;
    ehci->interrupt_qh->overlay.token    = (1 << 6);  /* Halted */

    for (int i = 0; i < 1024; i++) {
        ehci->periodic_list[i] = intr_phys | QH_PTR_TYPE_QH;
    }

    ehci->async_qh = (ehci_qh_t*)hal_alloc_dma_aligned(sizeof(ehci_qh_t), 64);
    uint32_t qh_phys = (uint32_t)hal_virt_to_phys(ehci->async_qh);

    ehci->async_qh->horizontal_link = qh_phys | QH_PTR_TYPE_QH;
    ehci->async_qh->characteristics = (1 << 15) | (1 << 14) | (2 << 12) | (64 << 16);
    ehci->async_qh->capabilities    = (1 << 30);
    ehci->async_qh->overlay.next     = QTD_PTR_INVALID;
    ehci->async_qh->overlay.alt_next = QTD_PTR_INVALID;
    ehci->async_qh->overlay.token    = (1 << 6);

    ehci->op_regs->periodiclistbase = (uint32_t)hal_virt_to_phys(ehci->periodic_list);
    ehci->op_regs->asyncestbase     = qh_phys;

    ehci->op_regs->usbcmd |= CMD_PERIODIC_ENABLE | CMD_ASYNC_ENABLE;
}

/* ============================================================================
 * qTD BUILDER & EXECUTION ENGINE
 * ============================================================================ */

static ehci_qtd_t* ehci_build_qtd_chain(void* buffer, uint32_t length, uint8_t pid, uint8_t* toggle) {
    uint32_t remaining = length;
    uint8_t* curr_buf = (uint8_t*)buffer;
    ehci_qtd_t* head_qtd = NULL;
    ehci_qtd_t* prev_qtd = NULL;

    do {
        ehci_qtd_t* qtd = (ehci_qtd_t*)hal_alloc_dma_aligned(sizeof(ehci_qtd_t), 32);
        if (!qtd) return NULL;

        if (!head_qtd) head_qtd = qtd;
        if (prev_qtd) {
            prev_qtd->next = (uint32_t)hal_virt_to_phys(qtd);
        }

        qtd->next = QTD_PTR_INVALID;
        qtd->alt_next = QTD_PTR_INVALID;

        uint32_t bytes_this_qtd = 0;
        uintptr_t virt_addr = (uintptr_t)curr_buf;

        for (int i = 0; i < 5 && remaining > 0; i++) {
            uint32_t phys_page = (uint32_t)hal_virt_to_phys((void*)virt_addr);
            qtd->buffers[i] = phys_page;

            uint32_t page_offset = virt_addr & 0xFFF;
            uint32_t space_in_page = PAGE_SIZE - page_offset;
            uint32_t chunk = (remaining < space_in_page) ? remaining : space_in_page;

            bytes_this_qtd += chunk;
            remaining -= chunk;
            curr_buf += chunk;
            virt_addr = (uintptr_t)curr_buf;

            if (bytes_this_qtd >= (16 * 1024)) break;
        }

        qtd->token = (pid << 8) | 
                     (bytes_this_qtd << 16) | 
                     ((uint32_t)(*toggle & 1) << 31) | 
                     (3 << 10) | 
                     (1 << 7); /* Active */

        *toggle ^= 1;
        prev_qtd = qtd;
    } while (remaining > 0);

    return head_qtd;
}

static bool ehci_execute_transfer(ehci_qh_t* qh, ehci_qtd_t* head_qtd) {
    uint32_t head_phys = (uint32_t)hal_virt_to_phys(head_qtd);

    qh->current_qtd  = 0;
    qh->overlay.next = head_phys;
    qh->overlay.alt_next = QTD_PTR_INVALID;
    qh->overlay.token &= ~(1 << 6); /* Clear Halted */
    qh->overlay.token &= ~(1 << 7); /* Clear Active on overlay */

    ehci_qtd_t* tail = head_qtd;
    while (!(tail->next & QTD_PTR_INVALID)) {
        tail = (ehci_qtd_t*)(uintptr_t)((tail->next & ~0x1F) + HHDM_OFFSET);
    }

    uint32_t timeout = 3000;
    while ((tail->token & (1 << 7)) && --timeout) {
        delay_ms(1);
    }

    bool success = (timeout > 0) && ((tail->token & 0xFC) == 0);
    if (!success) {
        printk(LOG_ERROR, "EHCI [Transfer]: Execution failed! Token Status: 0x%08x\n", tail->token);
    }

    return success;
}

/* ============================================================================
 * EHCI TRANSFERS
 * ============================================================================ */

bool ehci_control_transfer(uint8_t dev_addr, ehci_usb_setup_packet_t* setup, void* data) {
    if (!g_ehci.active) return false;

    ehci_usb_device_t* dev = &g_usb_devices[dev_addr];
    ehci_usb_device_t dummy_dev;

    if (dev_addr == 0 || !dev->connected) {
        memset(&dummy_dev, 0, sizeof(ehci_usb_device_t));
        dummy_dev.address = dev_addr;
        dummy_dev.speed   = USB_SPEED_HIGH;
        dev = &dummy_dev;
    }

    ehci_qtd_t* qtd_setup  = (ehci_qtd_t*)hal_alloc_dma_aligned(sizeof(ehci_qtd_t), 32);
    ehci_qtd_t* qtd_status = (ehci_qtd_t*)hal_alloc_dma_aligned(sizeof(ehci_qtd_t), 32);

    if (!qtd_setup || !qtd_status) return false;

    /* 1. Setup Phase */
    qtd_setup->next = QTD_PTR_INVALID;
    qtd_setup->alt_next = QTD_PTR_INVALID;
    qtd_setup->token = (USB_PID_SETUP << 8) | (8 << 16) | (0 << 31) | (3 << 10) | (1 << 7);
    qtd_setup->buffers[0] = (uint32_t)hal_virt_to_phys(setup);

    ehci_qtd_t* last_qtd = qtd_setup;

    /* 2. Data Phase */
    if (setup->length > 0 && data != NULL) {
        uint8_t toggle = 1;
        uint8_t pid = (setup->request_type & 0x80) ? USB_PID_IN : USB_PID_OUT;
        ehci_qtd_t* qtd_data = ehci_build_qtd_chain(data, setup->length, pid, &toggle);
        
        if (!qtd_data) return false;

        last_qtd->next = (uint32_t)hal_virt_to_phys(qtd_data);
        while (!(last_qtd->next & QTD_PTR_INVALID)) {
            last_qtd = (ehci_qtd_t*)(uintptr_t)((last_qtd->next & ~0x1F) + HHDM_OFFSET);
        }
    }

    /* 3. Status Phase */
    uint8_t status_pid = (setup->length == 0 || !(setup->request_type & 0x80)) ? USB_PID_IN : USB_PID_OUT;
    qtd_status->next = QTD_PTR_INVALID;
    qtd_status->alt_next = QTD_PTR_INVALID;
    qtd_status->token = (status_pid << 8) | (0 << 16) | (1ULL << 31) | (3 << 10) | (1 << 7);

    last_qtd->next = (uint32_t)hal_virt_to_phys(qtd_status);

    ehci_init_qh_for_ep(g_ehci.async_qh, dev, 0, (dev->max_packet_in ? dev->max_packet_in : 64), EP_TYPE_CONTROL);

    return ehci_execute_transfer(g_ehci.async_qh, qtd_setup);
}

usb_endpoint_t* ehci_get_endpoint(uint8_t dev_addr, uint8_t ep_addr) {
    ehci_usb_device_t* dev = &g_usb_devices[dev_addr];
    if (!dev->connected) return NULL;

    for (int i = 0; i < dev->num_endpoints; i++) {
        if (dev->endpoints[i].address == ep_addr) {
            return &dev->endpoints[i];
        }
    }
    return NULL;
}

bool ehci_transfer_io(uint8_t dev_addr, uint8_t ep_addr, void* buffer, uint32_t length) {
    if (!g_ehci.active || length == 0) return false;

    ehci_usb_device_t* dev = &g_usb_devices[dev_addr];
    usb_endpoint_t* ep = ehci_get_endpoint(dev_addr, ep_addr);
    if (!dev->connected || !ep) return false;

    if (!ep->qh) {
        ep->qh = (ehci_qh_t*)hal_alloc_dma_aligned(sizeof(ehci_qh_t), 64);
        if (!ep->qh) return false;

        uint8_t ep_num = ep_addr & 0x0F;
        ehci_init_qh_for_ep(ep->qh, dev, ep_num, ep->max_packet_size, ep->type);

        if (ep->type == EP_TYPE_INTERRUPT) {
            ep->qh->horizontal_link = g_ehci.interrupt_qh->horizontal_link;
            g_ehci.interrupt_qh->horizontal_link = (uint32_t)hal_virt_to_phys(ep->qh) | QH_PTR_TYPE_QH;
        } else {
            uint32_t old_async_next = g_ehci.async_qh->horizontal_link;
            ep->qh->horizontal_link = old_async_next;
            g_ehci.async_qh->horizontal_link = (uint32_t)hal_virt_to_phys(ep->qh) | QH_PTR_TYPE_QH;
        }
    }

    uint8_t pid = (ep_addr & 0x80) ? USB_PID_IN : USB_PID_OUT;
    ehci_qtd_t* qtd_chain = ehci_build_qtd_chain(buffer, length, pid, &ep->toggle);
    if (!qtd_chain) return false;

    return ehci_execute_transfer(ep->qh, qtd_chain);
}

bool ehci_bulk_transfer(uint8_t dev_addr, uint8_t ep_addr, void* data, uint32_t length, uint8_t* toggle, bool is_in) {
    (void)toggle;
    uint8_t target_ep = (ep_addr & 0x0F) | (is_in ? 0x80 : 0x00);
    return ehci_transfer_io(dev_addr, target_ep, data, length);
}

bool ehci_bulk_read(uint8_t dev_addr, uint8_t ep_in, void* buffer, uint32_t length) {
    return ehci_transfer_io(dev_addr, ep_in | 0x80, buffer, length);
}

bool ehci_bulk_write(uint8_t dev_addr, uint8_t ep_out, const void* buffer, uint32_t length) {
    return ehci_transfer_io(dev_addr, ep_out & 0x7F, (void*)buffer, length);
}

bool ehci_interrupt_transfer(uint8_t dev_addr, uint8_t ep_in, void* buffer, uint32_t length) {
    return ehci_transfer_io(dev_addr, ep_in | 0x80, buffer, length);
}

bool ehci_hid_get_report_descriptor(uint8_t dev_addr, uint8_t interface_num, void* descriptor_buf, uint16_t length) {
    ehci_usb_setup_packet_t setup;
    setup.request_type = 0x81;
    setup.request      = USB_REQ_GET_DESCRIPTOR;
    setup.value        = (USB_DESC_REPORT << 8);
    setup.index        = interface_num;
    setup.length       = length;

    return ehci_control_transfer(dev_addr, &setup, descriptor_buf);
}

/* ============================================================================
 * ENUMERATION & ROUTING ENGINE
 * ============================================================================ */

bool ehci_enumerate_device(uint8_t port_idx, uint8_t dev_addr, uint8_t speed, bool behind_hub, uint8_t hub_addr, uint8_t hub_port) {
    printk(LOG_INFO, "EHCI [Enum]: Enumerating Dev %d (Speed: %d, Behind Hub: %d)...\n", 
           dev_addr, speed, behind_hub);

    ehci_usb_device_t* dev = &g_usb_devices[dev_addr];
    memset(dev, 0, sizeof(ehci_usb_device_t));
    dev->address       = dev_addr;
    dev->speed         = speed;
    dev->is_behind_hub = behind_hub;
    dev->hub_address   = hub_addr;
    dev->hub_port_num  = hub_port;

    ehci_usb_setup_packet_t setup;
    ehci_usb_device_descriptor_t* dev_desc = (ehci_usb_device_descriptor_t*)hal_alloc_dma_aligned(64, 32);

    setup.request_type = 0x80;
    setup.request      = USB_REQ_GET_DESCRIPTOR;
    setup.value        = (USB_DESC_DEVICE << 8);
    setup.index        = 0;
    setup.length       = 18;

    if (!ehci_control_transfer(0, &setup, dev_desc)) {
        printk(LOG_ERROR, "EHCI [Enum]: Failed to read Device Descriptor at address 0!\n");
        return false;
    }

    dev->max_packet_in = dev_desc->bMaxPacketSize0;

    setup.request_type = 0x00;
    setup.request      = USB_REQ_SET_ADDRESS;
    setup.value        = dev_addr;
    setup.index        = 0;
    setup.length       = 0;
    
    if (!ehci_control_transfer(0, &setup, NULL)) {
        printk(LOG_ERROR, "EHCI [Enum]: SET_ADDRESS command failed!\n");
        return false;
    }
    delay_ms(10);

    uint8_t* cfg_buf = (uint8_t*)hal_alloc_dma_aligned(512, 32);
    setup.request_type = 0x80;
    setup.request      = USB_REQ_GET_DESCRIPTOR;
    setup.value        = (USB_DESC_CONFIG << 8);
    setup.index        = 0;
    setup.length       = 512;
    
    if (!ehci_control_transfer(dev_addr, &setup, cfg_buf)) {
        printk(LOG_ERROR, "EHCI [Enum]: Failed to fetch Configuration Descriptors!\n");
        return false;
    }

    ehci_usb_config_descriptor_t* cfg = (ehci_usb_config_descriptor_t*)cfg_buf;
    uint8_t* ptr = cfg_buf + cfg->bLength;

    dev->connected     = true;
    dev->num_endpoints = 0;

    while (ptr < cfg_buf + cfg->wTotalLength) {
        uint8_t desc_type = ptr[1];
        
        if (desc_type == USB_DESC_ENDPOINT) {
            ehci_usb_endpoint_descriptor_t* ep = (ehci_usb_endpoint_descriptor_t*)ptr;
            uint8_t ep_type = ep->bmAttributes & 0x03;

            if (ep_type == EP_TYPE_BULK) {
                if (ep->bEndpointAddress & 0x80) {
                    dev->ep_in = ep->bEndpointAddress;
                    dev->max_packet_in = ep->wMaxPacketSize;
                } else {
                    dev->ep_out = ep->bEndpointAddress;
                    dev->max_packet_out = ep->wMaxPacketSize;
                }
            }

            if (dev->num_endpoints < EHCI_MAX_ENDPOINTS) {
                usb_endpoint_t* endpoint = &dev->endpoints[dev->num_endpoints++];
                endpoint->address         = ep->bEndpointAddress;
                endpoint->type            = ep_type;
                endpoint->max_packet_size = ep->wMaxPacketSize;
                endpoint->interval        = ep->bInterval;
                endpoint->toggle          = 0;
                endpoint->qh              = NULL;
            }
        }
        ptr += ptr[0];
    }

    setup.request_type = 0x00;
    setup.request      = USB_REQ_SET_CONFIG;
    setup.value        = cfg->bConfigurationValue;
    setup.index        = 0;
    setup.length       = 0;
    ehci_control_transfer(dev_addr, &setup, NULL);

    printk(LOG_INFO, "EHCI [Enum]: Enumerated Address %d (Registered Endpoints: %d)\n",
           dev_addr, dev->num_endpoints);

    return true;
}

void ehci_scan_ports(void) {
    if (!g_ehci.active) return;

    printk(LOG_INFO, "EHCI [Scan]: Scanning all %d ports for connected devices...\n", g_ehci.num_ports);
    uint8_t next_address = 1;

    for (uint8_t i = 0; i < g_ehci.num_ports; i++) {
        uint32_t portsc = g_ehci.op_regs->portsc[i];

        if (!(portsc & PORT_POWER)) {
            g_ehci.op_regs->portsc[i] = (portsc & ~0x002E) | PORT_POWER;
            delay_ms(20);
            portsc = g_ehci.op_regs->portsc[i];
        }

        if (portsc & PORT_CONNECTION) {
            printk(LOG_INFO, "EHCI [Scan]: Peripheral detected on Root Port %d. Resetting...\n", i + 1);

            g_ehci.op_regs->portsc[i] = (portsc & ~0x002E) | PORT_RESET;
            delay_ms(50);
            g_ehci.op_regs->portsc[i] &= ~(PORT_RESET | 0x002E);
            delay_ms(10);

            portsc = g_ehci.op_regs->portsc[i];
            uint32_t line_status = (portsc >> 10) & 0x03;

            if (!(portsc & PORT_ENABLE) || line_status == 1) {
                printk(LOG_WARNING, "EHCI [Scan]: Low/Full-Speed device on Root Port %d. Releasing ownership to Companion...\n", i + 1);
                g_ehci.op_regs->portsc[i] |= PORT_OWNER;
            } else {
                ehci_enumerate_device(i, next_address++, USB_SPEED_HIGH, false, 0, 0);
            }
        }
    }
}

int ehci_find_devices_by_class(uint8_t target_class, ehci_usb_matched_device_t* out_devices, int max_results) {
    if (!g_ehci.active || !out_devices || max_results <= 0) {
        return 0;
    }

    int match_count = 0;

    for (uint8_t addr = 1; addr < EHCI_MAX_DEVICES; addr++) {
        ehci_usb_device_t* dev = &g_usb_devices[addr];
        if (!dev->connected) continue;

        ehci_usb_setup_packet_t setup;
        uint8_t* cfg_buf = (uint8_t*)hal_alloc_dma_aligned(512, 32);
        if (!cfg_buf) continue;

        setup.request_type = 0x80;
        setup.request      = USB_REQ_GET_DESCRIPTOR;
        setup.value        = (USB_DESC_CONFIG << 8);
        setup.index        = 0;
        setup.length       = 512;

        if (ehci_control_transfer(dev->address, &setup, cfg_buf)) {
            ehci_usb_config_descriptor_t* cfg = (ehci_usb_config_descriptor_t*)cfg_buf;
            uint8_t* ptr = cfg_buf + cfg->bLength;
            bool is_match = false;

            while (ptr < cfg_buf + cfg->wTotalLength) {
                uint8_t desc_type = ptr[1];

                if (desc_type == USB_DESC_INTERFACE) {
                    ehci_usb_interface_descriptor_t* iface = (ehci_usb_interface_descriptor_t*)ptr;
                    if (iface->bInterfaceClass == target_class || target_class == 0xFF) {
                        is_match = true;
                        out_devices[match_count].port            = dev->address;
                        out_devices[match_count].address         = dev->address;
                        out_devices[match_count].device_class    = iface->bInterfaceClass;
                        out_devices[match_count].device_subclass = iface->bInterfaceSubClass;
                        out_devices[match_count].device_protocol = iface->bInterfaceProtocol;
                        break;
                    }
                }
                ptr += ptr[0];
            }

            if (is_match) {
                match_count++;
                if (match_count >= max_results) {
                    hal_free_dma_aligned(cfg_buf);
                    break;
                }
            }
        }
        hal_free_dma_aligned(cfg_buf);
    }

    return match_count;
}

/* ============================================================================
 * INITIALIZATION & DRIVER ENTRYPOINTS
 * ============================================================================ */

bool ehci_init_device(uint8_t bus, uint8_t dev, uint8_t fun) {
    printk(LOG_TRACE, "EHCI [PCI]: Controller found on B%d:D%d:F%d\n", bus, dev, fun);

    uint16_t cmd = pci_read16(bus, dev, fun, 0x04);
    pci_write16(bus, dev, fun, 0x04, cmd | (1 << 1) | (1 << 2));

    uint32_t bar0 = pci_read32(bus, dev, fun, 0x10);
    uintptr_t phys_mmio = bar0 & 0xFFFFFFF0;

    g_ehci.cap_regs  = (ehci_cap_regs_t*)hal_mmio_map(phys_mmio, 0x1000);
    g_ehci.op_regs   = (ehci_op_regs_t*)((uintptr_t)g_ehci.cap_regs + g_ehci.cap_regs->caplength);
    g_ehci.num_ports = g_ehci.cap_regs->hcsparams & 0x0F;

    printk(LOG_INFO, "================ EHCI HOST CONTROLLER ================\n");
    printk(LOG_INFO, "Cap Regs Virt : %p\n", g_ehci.cap_regs);
    printk(LOG_INFO, "Op Regs Virt  : %p (Length offset: 0x%x)\n", g_ehci.op_regs, g_ehci.cap_regs->caplength);
    printk(LOG_INFO, "HCI Version   : 0x%04x\n", g_ehci.cap_regs->hciversion);
    printk(LOG_INFO, "HCSPARAMS     : 0x%08x (Total Ports: %u)\n", g_ehci.cap_regs->hcsparams, g_ehci.num_ports);

    ehci_bios_handoff(bus, dev, fun, g_ehci.cap_regs->hccparams);

    if (!ehci_reset(&g_ehci)) {
        return false;
    }

    g_ehci.op_regs->ctrldsegment = 0;
    g_ehci.op_regs->configflag = 1;

    ehci_setup_schedules(&g_ehci);

    g_ehci.op_regs->usbcmd |= CMD_RUN;
    g_ehci.active = true;

    ehci_scan_ports();

    printk(LOG_INFO, "====================================================\n");

    return true;
}

void ehci_init(void) {
    printk(LOG_TRACE, "EHCI: Scanning PCI bus for USB 2.0 Host Controllers...\n");

    for (uint16_t b = 0; b < 256; b++) {
        for (uint8_t d = 0; d < 32; d++) {
            for (uint8_t f = 0; f < 8; f++) {
                uint32_t class_code = pci_read32(b, d, f, 0x08);
                uint8_t base_class = (class_code >> 24) & 0xFF;
                uint8_t sub_class  = (class_code >> 16) & 0xFF;
                uint8_t prog_if    = (class_code >> 8)  & 0xFF;

                if (base_class == EHCI_PCI_CLASS &&
                    sub_class  == EHCI_PCI_SUBCLASS &&
                    prog_if    == EHCI_PCI_PROGIF) {

                    if (ehci_init_device(b, d, f)) {
                        return;
                    }
                }
            }
        }
    }

    printk(LOG_WARNING, "EHCI: No EHCI-compliant controller discovered on PCI bus.\n");
}