#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>
#include <stdbool.h>

#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_DEV_GPU   0x1050

// Device Status Flags
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FAILED      128

// Capability Types
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define VIRTIO_MAX_QUEUE_SIZE 256

// VirtIO 1.0 Modern PCI Common Config Register Layout (MMIO)
struct virtio_pci_common_cfg {
    uint32_t device_feature_select; /* read-write */
    uint32_t device_feature;        /* read-only */
    uint32_t driver_feature_select; /* read-write */
    uint32_t driver_feature;        /* read-write */
    uint16_t msix_config;           /* read-write */
    uint16_t num_queues;            /* read-only */
    uint8_t  device_status;         /* read-write */
    uint8_t  config_generation;     /* read-only */

    uint16_t queue_select;          /* read-write */
    uint16_t queue_size;            /* read-write */
    uint16_t queue_msix_vector;     /* read-write */
    uint16_t queue_enable;          /* read-write */
    uint16_t queue_notify_off;      /* read-only */

    /* Split 64-bit physical address registers into 32-bit halves */
    uint32_t queue_desc_lo;         /* read-write */
    uint32_t queue_desc_hi;         /* read-write */
    uint32_t queue_driver_lo;       /* read-write (Avail Ring) */
    uint32_t queue_driver_hi;       /* read-write */
    uint32_t queue_device_lo;       /* read-write (Used Ring)  */
    uint32_t queue_device_hi;       /* read-write */
} __attribute__((packed));

// Virtqueue Descriptor Layout
struct virtq_desc {
    uint64_t addr;   // Physical address
    uint32_t len;
    uint16_t flags;  // 1=NEXT, 2=WRITE
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_MAX_QUEUE_SIZE];
    uint16_t used_event;
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[VIRTIO_MAX_QUEUE_SIZE];
    uint16_t avail_event;
} __attribute__((packed));

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;

    volatile struct virtio_pci_common_cfg *common_cfg;
    volatile uint8_t *device_cfg;
    volatile uint8_t *isr_cfg;
    volatile uint16_t *notify_base;
    uint32_t notify_off_multiplier;

    // Queue pointers (Virtual addresses inside kernel space)
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    volatile struct virtq_used *used;
    uint16_t queue_size;
    uint16_t last_seen_used;
} virtio_device_t;

void virtio_set_hhdm_offset(uintptr_t offset);
bool virtio_init_device(virtio_device_t *dev, uint8_t bus, uint8_t device, uint8_t function);

bool virtio_send_command(virtio_device_t *dev,
                         void *out_data, uint32_t out_len,
                         void *in_data, uint32_t in_len);

#endif // VIRTIO_H