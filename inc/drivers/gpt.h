#pragma once

#include <stdint.h>
#include <drivers/ahci.h>

#define GPT_SIGNATURE 0x5452415020494645ULL // "EFI PART" in ASCII
#define MAX_VOLUMES 16

// GUID Structure definition used for unique identifiers
typedef struct {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} __attribute__((packed)) gpt_guid_t;

// GPT Main Header Structure (LBA 1)
typedef struct {
    uint64_t signature;               // Must match GPT_SIGNATURE
    uint32_t revision;                // Revision version (e.g., 0x00010000)
    uint32_t header_size;             // Header structure size (usually 92 bytes)
    uint32_t header_crc32;            // CRC32 checksum of the header itself
    uint32_t reserved0;               // Must be zero
    uint64_t my_lba;                  // LBA location of this header
    uint64_t alternate_lba;           // LBA location of the backup header
    uint64_t first_usable_lba;        // First data block allocation sector
    uint64_t last_usable_lba;         // Last data block allocation sector
    gpt_guid_t disk_guid;             // Unique ID for the disk
    uint64_t partition_entry_lba;     // Starting LBA of the entry array (usually 2)
    uint32_t num_partition_entries;   // Max partition entry count (usually 128)
    uint32_t size_of_partition_entry; // Size of each entry slot (usually 128 bytes)
    uint32_t partition_array_crc32;   // CRC32 checksum of the partition array
} __attribute__((packed)) gpt_header_t;

// GPT Partition Entry Structure (LBA 2 to 33)
typedef struct {
    gpt_guid_t partition_type_guid;   // Partition type classification identifier
    gpt_guid_t unique_partition_guid; // Unique ID for this specific partition instance
    uint64_t starting_lba;            // The starting sector address on the disk
    uint64_t ending_lba;              // The inclusive ending sector address
    uint64_t attributes;              // Attribute flags (e.g., read-only, bootable)
    uint16_t partition_name[36];      // String name encoded in UTF-16
} __attribute__((packed)) gpt_entry_t;

// Abstract storage layer structure for runtime environment partition tracking
typedef struct {
    ahci_device_t* drive;             // Linked underlying storage hardware device
    uint64_t start_lba;               // Absolute hardware block displacement position
    uint64_t total_sectors;           // Maximum continuous length block allocation span
    char name[36];                    // Standard null-terminated ASCII string name
    int is_valid;                     // Validation registration state flag
} volume_t;

/* --- Core Initialization and Discovery Prototypes --- */

/**
 * Scans a storage device to parse its GPT structures and registers active partitions.
 */
void gpt_parse_partitions(ahci_device_t* drive);

/**
 * Registers an isolated partition entry slice to the global system volume array tracker.
 */
void volume_register(ahci_device_t* drive, uint64_t start_lba, uint64_t total_sectors, const char* name);

/**
 * Retrieves a pointer to an active registered storage volume via its index context offset.
 */
volume_t* get_volume(int index);

/* --- Abstracted Partition Sector Storage I/O Prototypes --- */

/**
 * Reads data blocks relative to a target partition volume's local start position boundary.
 */
int volume_read_sectors(volume_t* vol, uint64_t relative_lba, uint16_t count, uint64_t buf_phys);

/**
 * Writes data blocks relative to a target partition volume's local start position boundary.
 */
int volume_write_sectors(volume_t* vol, uint64_t relative_lba, uint16_t count, uint64_t buf_phys);

/* --- Write Operations and Modification Prototypes --- */

/**
 * Overwrites target storage blocks to initialize a clean Protective MBR and blank GPT framework.
 * @return 1 on success, 0 on hardware write errors.
 */
int gpt_format_disk(ahci_device_t* drive);

/**
 * Finds the first available unallocated slot space to append a new partition entry.
 * @return 1 on success, 0 if out of slot space or if disk bounds are exceeded.
 */
int gpt_create_partition(ahci_device_t* drive, const char* name, uint64_t sector_count);

/**
 * Zeroes out a target partition slot index entry and recalibrates table validation checksum arrays.
 * @return 1 on success, 0 on invalid index parameter values or hardware error states.
 */
int gpt_delete_partition(ahci_device_t* drive, int index_slot);