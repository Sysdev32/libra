#pragma once
#include <stdint.h>
#include <stdbool.h>

#define EHCI_MAX_PORTS        16
#define EHCI_MAX_DEVICES      128
#define EHCI_MAX_ENDPOINTS    16

typedef struct {
    uint8_t  caplength;
    uint8_t  reserved;
    uint16_t hciversion;
    uint32_t hcsparams;
    uint32_t hccparams;
    uint64_t hcsp_portroute;
} __attribute__((packed)) ehci_cap_regs_t;

typedef struct {
    volatile uint32_t usbcmd;
    volatile uint32_t usbsts;
    volatile uint32_t usbintr;
    volatile uint32_t frindex;
    volatile uint32_t ctrldsegment;
    volatile uint32_t periodiclistbase;
    volatile uint32_t asyncestbase;
    volatile uint32_t reserved[9];
    volatile uint32_t configflag;
    volatile uint32_t portsc[];
} __attribute__((packed)) ehci_op_regs_t;

/* Queue Transfer Descriptor (qTD) - 32-byte aligned */
typedef struct ehci_qtd {
    uint32_t next;
    uint32_t alt_next;
    uint32_t token;
    uint32_t buffers[5];
    uint32_t ext_buffers[5];
} __attribute__((aligned(32), packed)) ehci_qtd_t;

/* Queue Head (QH) - 64-byte aligned (With Split Transaction Fields) */
typedef struct ehci_qh {
    uint32_t horizontal_link;
    uint32_t characteristics; /* DWord 1: EPS, Device Address, EP, Max Packet, Head Bit */
    uint32_t capabilities;    /* DWord 2: S-Mask, C-Mask, Hub Addr, Port Number, Mult */
    uint32_t current_qtd;
    ehci_qtd_t overlay;
} __attribute__((aligned(64), packed)) ehci_qh_t;

/* Setup Packet Header */
typedef struct {
    uint8_t  request_type;
    uint8_t  request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} __attribute__((packed)) usb_setup_packet_t;

/* Standard USB Descriptors */
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
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed)) usb_interface_descriptor_t;

typedef struct {
    uint8_t  port;
    uint8_t  address;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint16_t vendor_id;
    uint16_t product_id;
} usb_matched_device_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed)) usb_endpoint_descriptor_t;

typedef struct {
    uint8_t  bDescriptorType;
    uint16_t wDescriptorLength;
} __attribute__((packed)) hid_descriptor_optional_t;

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdHID;
    uint8_t  bCountryCode;
    uint8_t  bNumDescriptors;
    uint8_t  bReportDescriptorType;
    uint16_t wReportDescriptorLength;
} __attribute__((packed)) usb_hid_descriptor_t;

/* Endpoint Tracking Structure */
typedef struct {
    uint8_t    address;
    uint8_t    type;            /* EP_TYPE_BULK, EP_TYPE_INTERRUPT, etc. */
    uint16_t   max_packet_size;
    uint8_t    interval;
    uint8_t    toggle;
    ehci_qh_t* qh;
} usb_endpoint_t;

/* Device Tracking Structure (Includes Hub / Speed context for Split Transactions) */
typedef struct {
    uint8_t        address;
    uint8_t        speed;          /* 0 = Full, 1 = Low, 2 = High */
    bool           is_behind_hub;  /* Requires CSPLIT/SSPLIT when speed != High */
    uint8_t        hub_address;    /* High-Speed TT Hub Address */
    uint8_t        hub_port_num;   /* High-Speed TT Hub Port Number */
    uint8_t        ep_in;
    uint8_t        ep_out;
    uint16_t       max_packet_in;
    uint16_t       max_packet_out;
    uint8_t        toggle_in;
    uint8_t        toggle_out;
    bool           connected;
    uint8_t        num_endpoints;
    usb_endpoint_t endpoints[EHCI_MAX_ENDPOINTS];
} usb_device_t;

typedef struct {
    ehci_cap_regs_t* cap_regs;
    ehci_op_regs_t*  op_regs;
    uint32_t*        periodic_list;
    ehci_qh_t*       async_qh;
    ehci_qh_t*       interrupt_qh;
    uint8_t          num_ports;
    bool             active;
} ehci_controller_t;

void ehci_init(void);
void ehci_scan_ports(void);
int  ehci_find_devices_by_class(uint8_t target_class, usb_matched_device_t* out_devices, int max_results);
bool ehci_control_transfer(uint8_t dev_addr, usb_setup_packet_t* setup, void* data);
bool ehci_transfer_io(uint8_t dev_addr, uint8_t ep_addr, void* buffer, uint32_t length);
bool ehci_bulk_transfer(uint8_t dev_addr, uint8_t ep_addr, void* data, uint32_t length, uint8_t* toggle, bool is_in);
bool ehci_bulk_read(uint8_t dev_addr, uint8_t ep_in, void* buffer, uint32_t length);
bool ehci_bulk_write(uint8_t dev_addr, uint8_t ep_out, const void* buffer, uint32_t length);
bool ehci_interrupt_transfer(uint8_t dev_addr, uint8_t ep_in, void* buffer, uint32_t length);
bool ehci_hid_get_report_descriptor(uint8_t dev_addr, uint8_t interface_num, void* descriptor_buf, uint16_t length);
bool ehci_enumerate_device(uint8_t port_idx, uint8_t dev_addr, uint8_t speed, bool behind_hub, uint8_t hub_addr, uint8_t hub_port);