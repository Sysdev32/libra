#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * CONFIGURATION CONSTANTS
 * ============================================================================ */

#define XHCI_MAX_DEVICES        32
#define XHCI_MAX_PORTS          16

/* ============================================================================
 * TRB TYPES & CONTROL FLAGS
 * ============================================================================ */

#define TRB_TYPE_NORMAL          1
#define TRB_TYPE_SETUP_STAGE     2
#define TRB_TYPE_DATA_STAGE      3
#define TRB_TYPE_STATUS_STAGE    4
#define TRB_TYPE_LINK            6
#define TRB_TYPE_ENABLE_SLOT     9
#define TRB_TYPE_ADDRESS_DEVICE  11
#define TRB_TYPE_CONFIGURE_EP    12
#define TRB_TYPE_CONFIGURE_ENDPOINT TRB_TYPE_CONFIGURE_EP
#define TRB_TYPE_TRANSFER_EVENT  32
#define TRB_TYPE_CMD_COMPLETION  33
#define TRB_TYPE_PORT_STATUS     34

#define TRB_CYCLE                (1 << 0)
#define TRB_ENT                  (1 << 1)
#define TRB_ISP                  (1 << 2)
#define TRB_NS                   (1 << 3)
#define TRB_CH                   (1 << 4)
#define TRB_IOC                  (1 << 5)
#define TRB_IDT                  (1 << 6)

/* ============================================================================
 * STANDARD USB SPECIFICATION STRUCTURES
 * ============================================================================ */

typedef struct {
    uint8_t  request_type;
    uint8_t  request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} __attribute__((packed)) usb_setup_packet_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_device_descriptor_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed)) usb_config_descriptor_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed)) usb_interface_descriptor_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed)) usb_endpoint_descriptor_t;

/* ============================================================================
 * HARDWARE STRUCTURES (TRB, CONTEXTS, ERST)
 * ============================================================================ */

typedef struct {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((aligned(16), packed)) xhci_trb_t;

typedef struct {
    uint64_t ring_segment_base_address;
    uint32_t ring_segment_size;
    uint32_t reserved;
} __attribute__((aligned(64), packed)) xhci_erst_entry_t;

typedef struct {
    uint32_t info1;
    uint32_t info2;
    uint32_t tt_info;
    uint32_t state;
    uint32_t reserved[4];
} __attribute__((aligned(32), packed)) xhci_slot_context_t;

typedef struct {
    union {
        uint32_t ep_info1;
        struct {
            uint32_t ep_state       : 3;
            uint32_t reserved0      : 5;
            uint32_t mult           : 2;
            uint32_t max_p_streams  : 5;
            uint32_t lsa            : 1;
            uint32_t interval       : 8;
            uint32_t max_esit_payload_hi : 8;
        };
    };
    uint32_t ep_info2;
    uint64_t tr_dequeue_ptr;
    uint16_t avg_trb_length;
    uint16_t max_esit_payload_lo;
    uint32_t reserved1[2];
} __attribute__((aligned(32), packed)) xhci_ep_context_t;

typedef struct {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint32_t reserved[6];
} __attribute__((aligned(32), packed)) xhci_input_control_context_t;

/* ============================================================================
 * REGISTER MAPS
 * ============================================================================ */

typedef struct {
    uint8_t  caplength;
    uint8_t  reserved;
    uint16_t hciversion;
    uint32_t hcsparams1;
    uint32_t hcsparams2;
    uint32_t hcsparams3;
    uint32_t hccparams1;
    uint32_t dboff;
    uint32_t rtsoff;
    uint32_t hccparams2;
} __attribute__((packed)) xhci_cap_regs_t;

typedef struct {
    volatile uint32_t usbcmd;
    volatile uint32_t usbsts;
    volatile uint32_t pagesize;
    volatile uint32_t reserved1[2];
    volatile uint32_t dnctrl;
    volatile uint64_t crcr;
    volatile uint32_t reserved2[4];
    volatile uint64_t dcbaap;
    volatile uint32_t config;
} __attribute__((packed)) xhci_op_regs_t;

typedef struct {
    volatile uint32_t iman;
    volatile uint32_t imod;
    volatile uint32_t erstsz;
    volatile uint32_t reserved;
    volatile uint64_t erstba;
    volatile uint64_t erdp;
} __attribute__((packed)) xhci_interrupter_t;

/* ============================================================================
 * DRIVER MANAGEMENT DATA STRUCTURES
 * ============================================================================ */

typedef struct {
    xhci_trb_t* ring;
    uint32_t    enqueue_idx;
    uint32_t    dequeue_idx;
    uint8_t     cycle_state;
    uint32_t    size;
} xhci_ring_t;

typedef struct {
    xhci_cap_regs_t*    cap_regs;
    xhci_op_regs_t*     op_regs;
    volatile uint32_t*  doorbells;
    xhci_interrupter_t* interrupter0;
    
    uint64_t*           dcbaa;
    xhci_ring_t         cmd_ring;
    xhci_ring_t         event_ring;
    xhci_erst_entry_t*  erst;
    
    uint8_t             max_slots;
    uint8_t             max_ports;
    bool                active;
} xhci_controller_t;

typedef struct {
    uint8_t address;
    uint8_t speed;
    bool    connected;
} usb_device_t;

typedef struct {
    uint8_t port;
    uint8_t address;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
} usb_matched_device_t;

/* ============================================================================
 * PUBLIC DRIVER API
 * ============================================================================ */

/* System Core */
void xhci_init(void);
bool xhci_init_device(uint8_t bus, uint8_t dev, uint8_t fun);
void xhci_scan_ports(void);
bool xhci_enumerate_device(uint8_t port_idx, uint8_t speed);
bool xhci_configure_endpoint(uint8_t slot_id, uint8_t ep_addr, uint16_t max_packet_size);

/* Transfers */
bool xhci_control_transfer(uint8_t slot_id, usb_setup_packet_t* setup, void* data_buf);
bool xhci_transfer_io(uint8_t slot_id, uint8_t ep_addr, void* buffer, uint32_t length);
bool xhci_bulk_read(uint8_t slot_id, uint8_t ep_in, void* buffer, uint32_t length);
bool xhci_bulk_write(uint8_t slot_id, uint8_t ep_out, const void* buffer, uint32_t length);
bool xhci_interrupt_transfer(uint8_t slot_id, uint8_t ep_in, void* buffer, uint32_t length);

/* Query & Search */
int xhci_find_devices_by_class(uint8_t target_class, usb_matched_device_t* out_devices, int max_results);

#ifdef __cplusplus
}
#endif