#include <stdint.h>
#include <string.h> // For memset, memcpy
#include <stdbool.h>
#include <drivers/pci.h>
#include <drivers/ahci.h>
#include <drivers/fb.h>

/* Register offsets */
#define RTL8139_IDR0        0x00    // MAC address (6 bytes)
#define RTL8139_MAR0        0x08

#define RTL8139_TSD0        0x10
#define RTL8139_TSD1        0x14
#define RTL8139_TSD2        0x18
#define RTL8139_TSD3        0x1C

#define RTL8139_TSAD0       0x20
#define RTL8139_TSAD1       0x24
#define RTL8139_TSAD2       0x28
#define RTL8139_TSAD3       0x2C

#define RTL8139_RBSTART     0x30
#define RTL8139_CMD         0x37
#define RTL8139_CAPR        0x38
#define RTL8139_IMR         0x3C
#define RTL8139_ISR         0x3E
#define RTL8139_RCR         0x44
#define RTL8139_CONFIG1     0x52

/* Command Register bits */
#define RTL8139_CMD_BUFE    (1 << 0) // RX Buffer Empty
#define RTL8139_CMD_TE      (1 << 2) // Transmitter Enable
#define RTL8139_CMD_RE      (1 << 3) // Receiver Enable
#define RTL8139_CMD_RST     (1 << 4) // Software Reset

/* Interrupt Status/Mask bits */
#define RTL8139_ISR_ROK     (1 << 0) // Receive OK
#define RTL8139_ISR_RER     (1 << 1) // Receive Error
#define RTL8139_ISR_TOK     (1 << 2) // Transmit OK
#define RTL8139_ISR_TER     (1 << 3) // Transmit Error

/* RCR bits */
#define RTL8139_RCR_AAP     (1 << 0)
#define RTL8139_RCR_APM     (1 << 1)
#define RTL8139_RCR_AM      (1 << 2)
#define RTL8139_RCR_AB      (1 << 3)
#define RTL8139_RCR_WRAP    (1 << 7)

typedef struct {
    uint16_t io_base;
} rtl8139_t;

/* --- Global Device State Variables --- */
static rtl8139_t rtl_dev;
static uint8_t *rx_buffer;
static uint32_t rx_offset = 0;

extern pci_device_t* devices;
extern uint32_t devicecount;

struct __attribute__((packed)) eth_frame {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t ethertype;
    uint8_t payload[46]; 
};

struct __attribute__((packed)) rtl8139_rx_header {
    uint16_t status;
    uint16_t length;
};

/* --- Low-Level Port I/O Instructions --- */

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

/* --- Global Register Accessors with Verbose Logging --- */

static inline uint8_t rtl_read8(uint16_t reg) {
    uint8_t val = inb(rtl_dev.io_base + reg);
    printk(LOG_TRACE, "[RTL8139] READ8  offset 0x%02X -> value 0x%02X\n", reg, val);
    return val;
}

static inline uint16_t rtl_read16(uint16_t reg) {
    uint16_t val = inw(rtl_dev.io_base + reg);
    printk(LOG_TRACE, "[RTL8139] READ16 offset 0x%02X -> value 0x%04X\n", reg, val);
    return val;
}

static inline uint32_t rtl_read32(uint16_t reg) {
    uint32_t val = inl(rtl_dev.io_base + reg);
    printk(LOG_TRACE, "[RTL8139] READ32 offset 0x%02X -> value 0x%08X\n", reg, val);
    return val;
}

static inline void rtl_write8(uint16_t reg, uint8_t value) {
    printk(LOG_TRACE, "[RTL8139] WRITE8 offset 0x%02X <- value 0x%02X\n", reg, value);
    outb(rtl_dev.io_base + reg, value);
}

static inline void rtl_write16(uint16_t reg, uint16_t value) {
    printk(LOG_TRACE, "[RTL8139] WRITE16 offset 0x%02X <- value 0x%04X\n", reg, value);
    outw(rtl_dev.io_base + reg, value);
}

static inline void rtl_write32(uint16_t reg, uint32_t value) {
    printk(LOG_TRACE, "[RTL8139] WRITE32 offset 0x%02X <- value 0x%08X\n", reg, value);
    outl(rtl_dev.io_base + reg, value);
}

/* --- Global High-Level Drivers --- */

static inline void rtl_set_tx_addr(unsigned slot, uint32_t phys) {
    printk(LOG_TRACE, "[RTL8139] Setting TX Addr for slot %u to 0x%08X\n", slot, phys);
    rtl_write32(RTL8139_TSAD0 + slot * 4, phys);
}

static inline void rtl_start_tx(unsigned slot, uint16_t len) {
    printk(LOG_TRACE, "[RTL8139] Starting TX on slot %u with length %u bytes\n", slot, len);
    rtl_write32(RTL8139_TSD0 + slot * 4, len);
}

static inline uint32_t rtl_get_tx_status(unsigned slot) {
    return rtl_read32(RTL8139_TSD0 + slot * 4);
}   

static inline void rtl_power_on(void) {
    printk(LOG_TRACE, "[RTL8139] Powering on device via CONFIG1\n");
    rtl_write8(RTL8139_CONFIG1, 0x00);
}

static inline void rtl_reset(void) {
    printk(LOG_TRACE, "[RTL8139] Triggering software reset\n");
    rtl_write8(RTL8139_CMD, RTL8139_CMD_RST);
}

static inline void rtl_wait_reset(void) {
    printk(LOG_TRACE, "[RTL8139] Waiting for software reset to clear...\n");
    uint32_t loops = 0;
    while (rtl_read8(RTL8139_CMD) & RTL8139_CMD_RST) {
        loops++;
        if (loops % 100000 == 0) {
            printk(LOG_TRACE, "[RTL8139] Still waiting for reset... (%u checks)\n", loops);
        }
    }
    printk(LOG_TRACE, "[RTL8139] Reset complete after %u loop evaluations.\n", loops);
}

static inline void rtl_enable(void) {
    printk(LOG_TRACE, "[RTL8139] Enabling Receiver (RE) and Transmitter (TE)\n");
    rtl_write8(RTL8139_CMD, RTL8139_CMD_RE | RTL8139_CMD_TE);
}

static inline void rtl_set_rx_buffer(uint32_t phys) {
    printk(LOG_TRACE, "[RTL8139] Configuring RX Ring Buffer Physical Address: 0x%08X\n", phys);
    rtl_write32(RTL8139_RBSTART, phys);
}

static inline void rtl_set_rcr(uint32_t value) {
    printk(LOG_TRACE, "[RTL8139] Setting Receive Configuration Register (RCR) to 0x%08X\n", value);
    rtl_write32(RTL8139_RCR, value);
}

static inline void rtl_set_imr(uint16_t mask) {
    printk(LOG_TRACE, "[RTL8139] Setting Interrupt Mask Register (IMR) to 0x%04X\n", mask);
    rtl_write16(RTL8139_IMR, mask);
}

static inline uint16_t rtl_get_isr(void) {
    return rtl_read16(RTL8139_ISR);
}

static inline void rtl_ack_irq(uint16_t bits) {
    printk(LOG_TRACE, "[RTL8139] Acknowledging/Clearing Status bits: 0x%04X\n", bits);
    rtl_write16(RTL8139_ISR, bits);
}

/* --- Packet Transmission --- */
bool rtl8139_send(const void *packet, uint16_t len)
{
    static uint8_t tx_slot = 0;
    static void *tx_buffer[4] = {0};
    static uint32_t tx_phys[4] = {0};

    if (len > 1792) {
        printk(LOG_TRACE, "[RTL8139] TX ERROR: Packet size %u exceeds hardware limit.\n", len);
        return false;
    }

    /* Synchronous buffer setup on first use */
    if (!tx_buffer[0]) {
        for (int i = 0; i < 4; i++) {
            tx_buffer[i] = kmalloc_aligned(2048, 16);
            tx_phys[i] = (uint32_t)((uint64_t)tx_buffer[i] - HHDM_OFFSET);

            printk(LOG_TRACE,
                "[RTL8139] TX buffer %d allocated: virt=0x%016llX phys=0x%08X\n",
                i, (unsigned long long)tx_buffer[i], tx_phys[i]);
        }
    }

    /* Polling: Actively spin-wait until the specific TX slot is empty */
    printk(LOG_TRACE, "[RTL8139] Polling ownership for TX slot %u before copying data...\n", tx_slot);
    uint32_t loops = 0;
    while (!(rtl_get_tx_status(tx_slot) & (1 << 13))) {
        loops++;
        if (loops % 500000 == 0) {
            printk(LOG_TRACE, "[RTL8139] Still waiting for TX slot %u to clear... (Status: 0x%08X)\n", 
                   tx_slot, rtl_get_tx_status(tx_slot));
        }
    }

    memcpy(tx_buffer[tx_slot], packet, len);

    rtl_set_tx_addr(tx_slot, tx_phys[tx_slot]);
    rtl_start_tx(tx_slot, len);

    printk(LOG_TRACE, "[RTL8139] Sent packet using TX slot %u (%u bytes) after %u poll loops.\n",
        tx_slot, len, loops);

    tx_slot = (tx_slot + 1) & 3;
    return true;
}

/* --- Packet Reception Processing Ring --- */
void rtl8139_receive(uint8_t* buf)
{
    printk(LOG_TRACE, "[RTL8139] Processing receive buffer ring...\n");

    /* Spin until the hardware reports the buffer is completely empty */
    while (!(rtl_read8(RTL8139_CMD) & RTL8139_CMD_BUFE)) {
        struct rtl8139_rx_header *hdr = (struct rtl8139_rx_header *)(rx_buffer + rx_offset);
        uint16_t status = hdr->status;
        uint16_t len    = hdr->length;

        printk(LOG_TRACE, "[RTL8139] Packet located in ring! status=0x%04X len=%u at offset 0x%X\n",
            status, len, rx_offset);

        uint8_t *packet = (uint8_t *)(hdr + 1);

        /* Verbose payload packet HEX dump */
        printk(LOG_TRACE, "[RTL8139] Raw Payload Data Dump:\n");
        for (uint16_t i = 0; i < len; i++) {
            printk(LOG_TRACE, "%02X ", packet[i]);
            if ((i & 15) == 15)
                printk(LOG_TRACE, "\n");
        }
        buf = packet;
        printk(LOG_TRACE, "\n");

        /* Header (4 bytes) + frame length, aligned to next 4-byte boundary */
        rx_offset += len + 4;
        rx_offset = (rx_offset + 3) & ~3;

        /* Wrap boundary calculation for 8 KiB Ring structure */
        rx_offset &= 0x1FFF;

        /* Underflow protection calculation for the CAPR register update */
        uint32_t capr_val = (rx_offset >= 16) ? (rx_offset - 16) : (0x2000 + rx_offset - 16);
        rtl_write16(RTL8139_CAPR, (uint16_t)capr_val);
    }
    printk(LOG_TRACE, "[RTL8139] RX ring buffer complete / fully emptied.\n");
}

/* --- Global Polling Strategy Entry Point --- */
void rtl8139_poll(uint8_t* buf) 
{
    uint16_t isr_status = rtl_get_isr();

    if (isr_status == 0) {
        return; // Early return to avoid flooding trace logs when idle
    }

    printk(LOG_TRACE, "[RTL8139_POLL] Activity detected! Captured ISR flags: 0x%04X\n", isr_status);

    /* Direct Poll: Handle incoming packet buffer data changes */
    if (isr_status & RTL8139_ISR_ROK) {
        printk(LOG_TRACE, "[RTL8139_POLL] RX Ok (ROK) flag set. Dispatching receiver loop.\n");
        rtl8139_receive(buf);
        rtl_ack_irq(RTL8139_ISR_ROK);
    }

    /* Direct Poll: Handle transmission event closures */
    if (isr_status & RTL8139_ISR_TOK) {
        printk(LOG_TRACE, "[RTL8139_POLL] TX Ok (TOK) flag caught. Frame completely pushed out.\n");
        rtl_ack_irq(RTL8139_ISR_TOK);
    }

    /* Clean tracking indicators for errors caught during continuous heavy polling */
    if (isr_status & (RTL8139_ISR_RER | RTL8139_ISR_TER)) {
        printk(LOG_TRACE, "[RTL8139_POLL] WARNING: Detected transmission or reception error state: 0x%04X\n", isr_status);
        rtl_ack_irq(isr_status & (RTL8139_ISR_RER | RTL8139_ISR_TER));
    }
}

/* --- Main Initialization Driver --- */
void init_rtl8139(void) {
    pci_device_t device;
    memset(&device, 0, sizeof(pci_device_t));
    int found = 0;

    printk(LOG_TRACE, "[RTL8139] Starting initialization scan. Total PCI devices tracked: %u\n", devicecount);

    for (int i = 0; i < devicecount; i++) {
        printk(LOG_TRACE, "[RTL8139] Checking PCI device %d: Class 0x%02X, Subclass 0x%02X\n", 
               i, devices[i].class_code, devices[i].subclass);
               
        if (devices[i].class_code == 2 && devices[i].subclass == 0) {
            device = devices[i];
            found = 1;
            printk(LOG_TRACE, "[RTL8139] Match found! Bus: %u, Device: %u, Function: %u\n", 
                   device.bus, device.device, device.function);
            break; 
        }
    }

    if (!found) {
        printk(LOG_TRACE, "[RTL8139] CRITICAL: RTL8139 card matching Network/Ethernet class not found!\n");
        return;
    }

    printk(LOG_TRACE, "[RTL8139] Reading PCI BAR0 configuration space offset 0x10...\n");
    uint32_t bar0_val = pci_read32(device.bus, device.device, device.function, 0x10);
    printk(LOG_TRACE, "[RTL8139] Raw BAR0 value read: 0x%08X\n", bar0_val);

    // Stripping indicator bit 0 assignment straight to our global device instance configuration block
    rtl_dev.io_base = (uint16_t)(bar0_val & ~0x1); 
    printk(LOG_TRACE, "[RTL8139] Sanitized I/O base port assigned to global: 0x%04X\n", rtl_dev.io_base);

    uint32_t alloc_size = 8192 + 16 + 1500;
    printk(LOG_TRACE, "[RTL8139] Attempting to allocate aligned RX buffer. Size: %u bytes, Alignment: 8192 bytes\n", alloc_size);
    
    uint64_t rxv = (uint64_t)kmalloc_aligned(alloc_size, 8192);
    printk(LOG_TRACE, "[RTL8139] Allocated RX Buffer Virtual Address: 0x%016llX\n", (unsigned long long)rxv);

    uint64_t rxp = rxv - HHDM_OFFSET;
    printk(LOG_TRACE, "[RTL8139] Computed RX Buffer Physical Address (using HHDM_OFFSET): 0x%016llX\n", (unsigned long long)rxp);
    rx_buffer = (uint8_t *)rxv;

    // Direct hardware setup steps via global state engine
    rtl_power_on();
    rtl_reset();
    rtl_wait_reset();
    
    rtl_set_rx_buffer((uint32_t)rxp);
    
    uint32_t rcr_flags = RTL8139_RCR_AAP  | 
                         RTL8139_RCR_APM  | 
                         RTL8139_RCR_AM   | 
                         RTL8139_RCR_AB   | 
                         RTL8139_RCR_WRAP;
    rtl_set_rcr(rcr_flags);
    
    /* PURE POLLING COMPLIANCE: Set mask bits register explicitly to zero */
    printk(LOG_TRACE, "[RTL8139] Disabling hardware interrupt line activations via IMR clear...\n");
    rtl_set_imr(0x0000);

    rtl_enable();

    // Verify system initialization via tracking address prints
    uint32_t mac_low = rtl_read32(RTL8139_IDR0);
    uint16_t mac_high = rtl_read16(RTL8139_IDR0 + 4);
    printk(LOG_TRACE, "[RTL8139] Initialization successful! Device MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\n",
           (uint8_t)(mac_low & 0xFF), 
           (uint8_t)((mac_low >> 8) & 0xFF), 
           (uint8_t)((mac_low >> 16) & 0xFF), 
           (uint8_t)((mac_low >> 24) & 0xFF),
           (uint8_t)(mac_high & 0xFF), 
           (uint8_t)((mac_high >> 8) & 0xFF));

    struct eth_frame frame = {0};
    uint8_t mac[6];

    mac[0] = (uint8_t)(mac_low);
    mac[1] = (uint8_t)(mac_low >> 8);
    mac[2] = (uint8_t)(mac_low >> 16);
    mac[3] = (uint8_t)(mac_low >> 24);
    mac[4] = (uint8_t)(mac_high);
    mac[5] = (uint8_t)(mac_high >> 8);

    // Broadcast frame details
    memset(frame.dst, 0xFF, sizeof(frame.dst));
    memcpy(frame.src, mac, sizeof(frame.src));
    frame.ethertype = 0xB588; 
    memcpy(frame.payload, "1234", 4);
    
    printk(LOG_TRACE, "[RTL8139] Dispatching validation diagnostic test packet...\n");
    rtl8139_send(&frame, sizeof(struct eth_frame));
}