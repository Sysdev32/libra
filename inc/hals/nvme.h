#ifndef NVME_H
#define NVME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ============================================================================
 * PCI CLASS & SUBCLASS CONSTANTS FOR NVME
 * ============================================================================ */

#define NVME_CLASS_CODE          0x01  /* Mass Storage Controller */
#define NVME_SUBCLASS            0x08  /* Non-Volatile Memory Subsystem */
#define NVME_PROG_IF              0x02  /* NVM Express Interface */

#define NVME_PCI_CLASS           NVME_CLASS_CODE
#define NVME_PCI_SUBCLASS        NVME_SUBCLASS
#define NVME_PCI_PROGIF          NVME_PROG_IF

#define NVME_MAX_DEVICES         8
#define QUEUE_DEPTH              64
#define NVME_DEFAULT_SECTOR_SIZE 512

/* ============================================================================
 * NVME OPCODES
 * ============================================================================ */

#define NVME_ADMIN_DELETE_IO_SQ  0x00
#define NVME_ADMIN_CREATE_IO_SQ  0x01
#define NVME_ADMIN_DELETE_IO_CQ  0x04
#define NVME_ADMIN_CREATE_IO_CQ  0x05
#define NVME_ADMIN_IDENTIFY     0x06

#define NVME_CMD_WRITE           0x01
#define NVME_CMD_READ            0x02

/* ============================================================================
 * NVME CONTROLLER MMIO REGISTERS
 * ============================================================================ */

typedef struct {
    volatile uint64_t cap;        /* 0x00: Controller Capabilities */
    volatile uint32_t vs;         /* 0x08: Version */
    volatile uint32_t intms;      /* 0x0C: Interrupt Mask Set */
    volatile uint32_t intmc;      /* 0x10: Interrupt Mask Clear */
    volatile uint32_t cc;         /* 0x14: Controller Configuration */
    volatile uint32_t reserved0;  /* 0x18: Reserved */
    volatile uint32_t csts;       /* 0x1C: Controller Status */
    volatile uint32_t nssr;       /* 0x20: NVM Subsystem Reset (Optional) */
    volatile uint32_t aqa;        /* 0x24: Admin Queue Attributes */
    volatile uint64_t asq;        /* 0x28: Admin Submission Queue Base Address */
    volatile uint64_t acq;        /* 0x30: Admin Completion Queue Base Address */
    volatile uint32_t cmbloc;     /* 0x38: Controller Memory Buffer Location */
    volatile uint32_t cmbsz;      /* 0x3C: Controller Memory Buffer Size */
} __attribute__((packed)) nvme_bar_regs_t;

typedef nvme_bar_regs_t nvme_regs_t;

/* ============================================================================
 * QUEUE ENTRIES
 * ============================================================================ */

typedef struct {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t command_id;
    uint32_t nsid;
    uint64_t reserved0;
    uint64_t mptr;          /* Metadata Pointer */
    uint64_t prp1;          /* Physical Region Page Entry 1 */
    uint64_t prp2;          /* Physical Region Page Entry 2 */
    
    /* Command Specific DWORDs */
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_sqe_t;

typedef nvme_sqe_t nvme_sq_entry_t;

typedef struct {
    uint32_t cdw0;          /* Command-specific result */
    uint32_t reserved;
    uint16_t sq_head;       /* Current SQ head pointer */
    uint16_t sq_id;         /* SQ Identifier */
    uint16_t command_id;    /* ID matching original command */
    uint16_t status;        /* Phase Tag bit [0], Status Code bits [15:1] */
} __attribute__((packed)) nvme_cqe_t;

typedef nvme_cqe_t nvme_cq_entry_t;

/* ============================================================================
 * IDENTIFY STRUCTURES
 * ============================================================================ */

typedef struct {
    uint16_t vid;           /* PCI Vendor ID */
    uint16_t ssvid;         /* PCI Subsystem Vendor ID */
    char     sn[20];        /* Serial Number */
    char     mn[40];        /* Model Number */
    char     fr[8];         /* Firmware Revision */
    uint8_t  rab;           /* Recommended Arbitration Burst */
    uint8_t  ieee[3];       /* IEEE OUI Identifier */
    uint8_t  cmic;          /* Controller Multi-Path I/O Capabilities */
    uint8_t  mdts;          /* Maximum Data Transfer Size (Power of 2) */
    uint16_t cntlid;        /* Controller ID */
    uint32_t ver;           /* Version */
    uint8_t  reserved[172];
    uint16_t oaes;          /* Optional Asynchronous Events Supported */
    uint32_t ctratt;        /* Controller Attributes */
    uint8_t  reserved1[12];
    uint8_t  nn;            /* Number of Namespaces */
    uint8_t  reserved2[2791];
} __attribute__((packed, aligned(4096))) nvme_identify_ctrl_t;

typedef struct {
    uint16_t lbads;         /* LBA Data Size (2^lbads) */
    uint16_t ms;            /* Metadata Size */
    uint8_t  rp;            /* Relative Performance */
    uint8_t  reserved;
} __attribute__((packed)) nvme_lbaf_t;

typedef struct {
    uint64_t nsze;          /* Namespace Size (Total Logical Blocks) */
    uint64_t ncap;          /* Namespace Capacity */
    uint64_t nuse;          /* Namespace Utilization */
    uint8_t  nsfeat;        /* Namespace Features */
    uint8_t  nlbaf;         /* Number of LBA Formats */
    uint8_t  flbas;         /* Formatted LBA Size */
    uint8_t  mc;            /* Metadata Capabilities */
    uint8_t  dpc;           /* End-to-end Data Protection Capabilities */
    uint8_t  dps;           /* End-to-end Data Protection Type Settings */
    uint8_t  nmic;          /* Namespace Multi-path I/O Capabilities */
    uint8_t  rescap;        /* Reservation Capabilities */
    uint8_t  reserved[119];
    nvme_lbaf_t lbaf[16];   /* LBA Format Support Tables */
    uint8_t  reserved2[3904];
} __attribute__((packed, aligned(4096))) nvme_identify_ns_t;

/* ============================================================================
 * NVME DEVICE INSTANCE STATE
 * ============================================================================ */

typedef struct {
    uint32_t         id;
    uint64_t         bar0;
    volatile nvme_regs_t* regs;
    uint32_t         db_stride;

    /* Admin Queue Pairs */
    nvme_sqe_t*      admin_sq;
    nvme_cqe_t*      admin_cq;
    uint16_t         admin_sq_tail;
    uint16_t         admin_cq_head;
    uint8_t          admin_phase;

    /* I/O Queue Pairs */
    nvme_sqe_t*      io_sq;
    nvme_cqe_t*      io_cq;
    uint16_t         io_sq_tail;
    uint16_t         io_cq_head;
    uint8_t          io_phase;

    /* Identified Target Metadata */
    uint32_t         nsid;
    uint64_t         total_sectors;
    uint32_t         sector_size;

    uint16_t         cmd_id_counter;
    bool             active;
} nvme_device_t;

typedef struct {
    nvme_device_t*   devices;
    uint32_t         count;
} nvme_list_t;

/* ============================================================================
 * PUBLIC DRIVER API
 * ============================================================================ */

nvme_list_t nvme_init(void);
bool nvme_read_block(uint32_t nvme_id, uint32_t nsid, uint64_t lba, uint16_t sector_count, void* buffer);
bool nvme_write_block(uint32_t nvme_id, uint32_t nsid, uint64_t lba, uint16_t sector_count, const void* buffer);
void nvme_close(void);

#endif /* NVME_H */