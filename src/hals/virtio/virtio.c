#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <hals/virtio/virtio.h>
#include <hals/pci.h>
#include <hals/ahci.h>
#include <drivers/fb.h>
#ifndef VMM_FLAGS_MMIO
#define PTE_PRESENT         (1ULL << 0)
#define PTE_WRITABLE        (1ULL << 1)
#define PTE_CACHE_DISABLE   (1ULL << 4)
#define VMM_FLAGS_MMIO      (PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE)
#endif
#define MAX_QUEUE_SIZE 256
#define PAGE_SIZE      4096

#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

typedef void page_table_t;
extern void vmm_map_page(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
extern page_table_t *vmm_get_current_pml4(void);
extern void *pmm_alloc_pages(int order);

static uintptr_t g_hhdm_offset = HHDM_OFFSET;

void virtio_set_hhdm_offset(const uintptr_t offset) {
    g_hhdm_offset = offset;
}

static uint64_t pci_get_bar_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index) {
    if (bar_index > 5) return 0;

    uint8_t offset = 0x10 + (bar_index * 4);
    uint32_t bar_low = pci_read32(bus, dev, func, offset);

    if (bar_low & 0x1) return 0; // Skip IO space BARs

    uint32_t bar_type = (bar_low >> 1) & 0x3;
    uint64_t bar_phys = (uint64_t)(bar_low & ~0x0FULL);

    if (bar_type == 0x2) { // 64-bit BAR
        uint32_t bar_high = pci_read32(bus, dev, func, offset + 4);
        bar_phys |= ((uint64_t)bar_high << 32);
    }

    return bar_phys;
}

static void virtio_parse_capabilities(virtio_device_t *dev) {

    uint8_t cap_ptr = pci_read8(dev->bus, dev->device, dev->function, 0x34);
    page_table_t *pml4 = vmm_get_current_pml4();

    while (cap_ptr != 0) {
        uint8_t cap_vndr = pci_read8(dev->bus, dev->device, dev->function, cap_ptr + 0);

        if (cap_vndr == 0x09) { // VirtIO Vendor Capability
            uint8_t cfg_type = pci_read8(dev->bus, dev->device, dev->function, cap_ptr + 3);
            uint8_t bar      = pci_read8(dev->bus, dev->device, dev->function, cap_ptr + 4);
            uint32_t offset  = pci_read32(dev->bus, dev->device, dev->function, cap_ptr + 8);
            uint32_t length  = pci_read32(dev->bus, dev->device, dev->function, cap_ptr + 12);

            uint64_t bar_phys = pci_get_bar_addr(dev->bus, dev->device, dev->function, bar);

            if (bar_phys != 0) {
                uint64_t phys_start = bar_phys + offset;
                uint64_t virt_start = phys_start + g_hhdm_offset;

                uint64_t page_phys = phys_start & ~(PAGE_SIZE - 1);
                uint64_t page_virt = virt_start & ~(PAGE_SIZE - 1);
                size_t total_length = (phys_start - page_phys) + length;

                for (size_t off = 0; off < total_length; off += PAGE_SIZE) {
                    vmm_map_page(pml4, page_virt + off, page_phys + off, VMM_FLAGS_MMIO);
                }

                void *mmio_virt = (void *)(uintptr_t)virt_start;

                switch (cfg_type) {
                    case VIRTIO_PCI_CAP_COMMON_CFG:
                        dev->common_cfg = (volatile struct virtio_pci_common_cfg *)mmio_virt;
                        break;
                    case VIRTIO_PCI_CAP_DEVICE_CFG:
                        dev->device_cfg = (volatile uint8_t *)mmio_virt;
                        break;
                    case VIRTIO_PCI_CAP_ISR_CFG:
                        dev->isr_cfg = (volatile uint8_t *)mmio_virt;
                        break;
                    case VIRTIO_PCI_CAP_NOTIFY_CFG:
                        dev->notify_base = (volatile uint16_t *)mmio_virt;
                        dev->notify_off_multiplier = pci_read32(dev->bus, dev->device, dev->function, cap_ptr + 16);
                        break;
                }
            }
        }

        cap_ptr = pci_read8(dev->bus, dev->device, dev->function, cap_ptr + 1);
    }
}

static void virtio_dump_queue_state(virtio_device_t *dev) {
}

static void virtio_setup_queue(virtio_device_t *dev, uint16_t q_index) {
    printk(LOG_TRACE, "[virtio-pci] Configuring VirtQueue %u...\n", q_index);

    dev->common_cfg->queue_select = q_index;
    __asm__ volatile("mfence" ::: "memory");

    uint16_t max_size = dev->common_cfg->queue_size;
    dev->queue_size = (max_size > MAX_QUEUE_SIZE) ? MAX_QUEUE_SIZE : max_size;
    dev->common_cfg->queue_size = dev->queue_size;

    // Dynamically allocate HHDM-backed physical pages via PMM
    void *desc_virt  = pmm_alloc_pages(0);
    void *avail_virt = pmm_alloc_pages(0);
    void *used_virt  = pmm_alloc_pages(0);

    memset(desc_virt, 0, PAGE_SIZE);
    memset(avail_virt, 0, PAGE_SIZE);
    memset(used_virt, 0, PAGE_SIZE);

    dev->desc  = (struct virtq_desc *)desc_virt;
    dev->avail = (struct virtq_avail *)avail_virt;
    dev->used  = (volatile struct virtq_used *)used_virt;

    dev->avail->flags = 0;
    dev->avail->idx   = 0;
    dev->last_seen_used = 0;

    // Calculate valid physical addresses from HHDM virtual addresses
    uint64_t desc_phys  = (uint64_t)desc_virt - g_hhdm_offset;
    uint64_t avail_phys = (uint64_t)avail_virt - g_hhdm_offset;
    uint64_t used_phys  = (uint64_t)used_virt - g_hhdm_offset;

    dev->common_cfg->queue_desc_lo   = (uint32_t)(desc_phys & 0xFFFFFFFF);
    dev->common_cfg->queue_desc_hi   = (uint32_t)(desc_phys >> 32);

    dev->common_cfg->queue_driver_lo = (uint32_t)(avail_phys & 0xFFFFFFFF);
    dev->common_cfg->queue_driver_hi = (uint32_t)(avail_phys >> 32);

    dev->common_cfg->queue_device_lo = (uint32_t)(used_phys & 0xFFFFFFFF);
    dev->common_cfg->queue_device_hi = (uint32_t)(used_phys >> 32);

    __asm__ volatile("mfence" ::: "memory");
    dev->common_cfg->queue_enable = 1;
    __asm__ volatile("mfence" ::: "memory");
}

bool virtio_init_device(virtio_device_t *dev, uint8_t bus, uint8_t device, uint8_t function) {
    dev->bus = bus;
    dev->device = device;
    dev->function = function;

    uint16_t pci_cmd = pci_read16(bus, device, function, 0x04);
    pci_write16(bus, device, function, 0x04, pci_cmd | 0x06);

    virtio_parse_capabilities(dev);

    if (!dev->common_cfg) return false;

    // Reset and acknowledge device
    dev->common_cfg->device_status = 0;
    __asm__ volatile("mfence" ::: "memory");

    dev->common_cfg->device_status |= VIRTIO_STATUS_ACKNOWLEDGE;
    __asm__ volatile("mfence" ::: "memory");

    dev->common_cfg->device_status |= VIRTIO_STATUS_DRIVER;
    __asm__ volatile("mfence" ::: "memory");

    // Negotiate features
    dev->common_cfg->device_feature_select = 0;
    uint32_t features = dev->common_cfg->device_feature;

    dev->common_cfg->driver_feature_select = 0;
    dev->common_cfg->driver_feature = features;
    __asm__ volatile("mfence" ::: "memory");

    dev->common_cfg->device_status |= VIRTIO_STATUS_FEATURES_OK;
    __asm__ volatile("mfence" ::: "memory");

    if (!(dev->common_cfg->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        dev->common_cfg->device_status |= VIRTIO_STATUS_FAILED;
        return false;
    }

    virtio_setup_queue(dev, 0);

    dev->common_cfg->device_status |= VIRTIO_STATUS_DRIVER_OK;
    __asm__ volatile("mfence" ::: "memory");

    printk(LOG_TRACE, "[virtio-pci] Device initialized successfully!\n");
    return true;
}

static inline void virtio_notify(virtio_device_t *dev, uint16_t q_index) {
    dev->common_cfg->queue_select = q_index;
    __asm__ volatile("mfence" ::: "memory");

    uint16_t notify_off = dev->common_cfg->queue_notify_off;
    uintptr_t doorbell_addr = (uintptr_t)dev->notify_base + ((uint32_t)notify_off * dev->notify_off_multiplier);
    volatile uint16_t *doorbell = (volatile uint16_t *)doorbell_addr;

    *doorbell = q_index;
    __asm__ volatile("mfence" ::: "memory");
}

bool virtio_send_command(virtio_device_t *dev,
                         void *out_data, uint32_t out_len,
                         void *in_data, uint32_t in_len) {

    if (!dev || !dev->desc || !out_data || out_len == 0 || out_len > PAGE_SIZE) {
        return false;
    }

    // Allocate dedicated bounce buffers from PMM to guarantee valid physical memory
    void *dma_out_virt = pmm_alloc_pages(0);
    void *dma_in_virt  = (in_data && in_len > 0) ? pmm_alloc_pages(0) : NULL;

    memset(dma_out_virt, 0, PAGE_SIZE);
    memcpy(dma_out_virt, out_data, out_len);

    if (dma_in_virt) {
        memset(dma_in_virt, 0, PAGE_SIZE);
    }

    uint64_t dma_out_phys = (uint64_t)dma_out_virt - g_hhdm_offset;
    uint64_t dma_in_phys  = dma_in_virt ? ((uint64_t)dma_in_virt - g_hhdm_offset) : 0;

    dev->desc[0].addr  = dma_out_phys;
    dev->desc[0].len   = out_len;
    dev->desc[0].flags = (dma_in_virt) ? VIRTQ_DESC_F_NEXT : 0;
    dev->desc[0].next  = 1;

    if (dma_in_virt) {
        dev->desc[1].addr  = dma_in_phys;
        dev->desc[1].len   = in_len;
        dev->desc[1].flags = VIRTQ_DESC_F_WRITE;
        dev->desc[1].next  = 0;
    }

    uint16_t avail_idx  = dev->avail->idx;
    uint16_t avail_slot = avail_idx % dev->queue_size;

    dev->avail->ring[avail_slot] = 0;

    __asm__ volatile("mfence" ::: "memory");
    dev->avail->idx = avail_idx + 1;
    __asm__ volatile("mfence" ::: "memory");

    virtio_dump_queue_state(dev);
    virtio_notify(dev, 0);

    while (dev->last_seen_used == dev->used->idx) {
        __asm__ volatile("pause");
    }

    if (in_data && dma_in_virt) {
        memcpy(in_data, dma_in_virt, in_len);
    }

    dev->last_seen_used++;
    return true;
}