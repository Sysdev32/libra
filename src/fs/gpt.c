#include <fs/gpt.h>
#include <drivers/fb.h>
#include <drivers/alloc.h> // kmalloc and kfree
#include <string.h>
#include <hals/ahci.h>
#include <hals/nvme.h>

static volume_t system_volumes[MAX_VOLUMES];
static int volume_count = 0;

// Simple CRC32 implementation commonly used in EFI/GPT specifications
static uint32_t crc32(const void* data, size_t length) {
    const uint8_t* byte = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= byte[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

// Low-level abstraction layer to unify AHCI and NVMe I/O reads
static int raw_drive_read(generic_drive_t* drive, uint64_t lba, uint16_t count, void* buf_virt) {
    if (!drive) return 0;

    if (drive->type == DRIVE_TYPE_AHCI) {
        if (!drive->ahci_drive || !drive->ahci_drive->is_initialized) return 0;
        uint64_t buf_phys = (uint64_t)buf_virt - (uint64_t)HHDM_OFFSET;
        return ahci_read_sectors(drive->ahci_drive, lba, count, buf_phys);
    } else if (drive->type == DRIVE_TYPE_NVME) {
        return nvme_read_block(drive->nvme.nvme_id, drive->nvme.nsid, lba, count, buf_virt) ? 1 : 0;
    }

    return 0;
}

// Low-level abstraction layer to unify AHCI and NVMe I/O writes
static int raw_drive_write(generic_drive_t* drive, uint64_t lba, uint16_t count, const void* buf_virt) {
    if (!drive) return 0;

    if (drive->type == DRIVE_TYPE_AHCI) {
        if (!drive->ahci_drive || !drive->ahci_drive->is_initialized) return 0;
        uint64_t buf_phys = (uint64_t)buf_virt - (uint64_t)HHDM_OFFSET;
        return ahci_write_sectors(drive->ahci_drive, lba, count, buf_phys);
    } else if (drive->type == DRIVE_TYPE_NVME) {
        return nvme_write_block(drive->nvme.nvme_id, drive->nvme.nsid, lba, count, buf_virt) ? 1 : 0;
    }

    return 0;
}

void volume_register(generic_drive_t drive, uint64_t start_lba, uint64_t total_sectors, const char* name) {
    if (volume_count >= MAX_VOLUMES) {
        printk(LOG_DEBUG, "[VOLUME ERROR] Maximum volume limit reached. Cannot register %s.\n", name);
        return;
    }

    system_volumes[volume_count].drive = drive;
    system_volumes[volume_count].start_lba = start_lba;
    system_volumes[volume_count].total_sectors = total_sectors;
    system_volumes[volume_count].is_valid = 1;

    strncpy(system_volumes[volume_count].name, name, 35);
    system_volumes[volume_count].name[35] = '\0';

    printk(LOG_DEBUG, "[VOLUME] Registered vol%d: '%s' | Start LBA: %llu | Sectors: %llu\n",
           volume_count, system_volumes[volume_count].name, start_lba, total_sectors);

    volume_count++;
}

volume_t* get_volume(int index) {
    if (index < 0 || index >= MAX_VOLUMES) return NULL;
    if (!system_volumes[index].is_valid) return NULL;
    return &system_volumes[index];
}

int volume_read_sectors(volume_t* vol, uint64_t relative_lba, uint16_t count, void* buf_virt) {
    if (!vol || !vol->is_valid) return 0;
    if (relative_lba + count > vol->total_sectors) {
        printk(LOG_DEBUG, "[VOLUME IO ERROR] Attempted to read past partition boundaries!\n");
        return 0;
    }
    uint64_t absolute_lba = vol->start_lba + relative_lba;
    return raw_drive_read(&vol->drive, absolute_lba, count, buf_virt);
}

int volume_write_sectors(volume_t* vol, uint64_t relative_lba, uint16_t count, const void* buf_virt) {
    if (!vol || !vol->is_valid) return 0;
    if (relative_lba + count > vol->total_sectors) {
        printk(LOG_DEBUG, "[VOLUME IO ERROR] Attempted to write past partition boundaries!\n");
        return 0;
    }
    uint64_t absolute_lba = vol->start_lba + relative_lba;
    return raw_drive_write(&vol->drive, absolute_lba, count, buf_virt);
}

// Helper function to verify if a GUID is unused (all zeroes)
static inline int is_guid_null(const gpt_guid_t* guid) {
    const uint8_t* p = (const uint8_t*)guid;
    for (int i = 0; i < 16; i++) {
        if (p[i] != 0) return 0; // Not null
    }
    return 1; // Unused slot
}

partition_table_t gpt_parse_partitions(generic_drive_t* drive) {
    partition_table_t table;
    memset(&table, 0, sizeof(partition_table_t));

    if (!drive) {
        printk(LOG_ERROR, "[GPT PARSE] NULL drive context provided.\n");
        return table;
    }

    uint32_t sector_size = (drive->sector_size > 0) ? drive->sector_size : 512;
    printk(LOG_INFO, "[GPT PARSE] Target Sector Size: %u bytes\n", sector_size);

    // Reset global volume state for this parse pass
    volume_count = 0;
    memset(system_volumes, 0, sizeof(system_volumes));

    void* sector_buffer_virt = kmalloc(sector_size);
    if (!sector_buffer_virt) {
        printk(LOG_ERROR, "[GPT PARSE] Failed to allocate LBA 1 header buffer.\n");
        return table;
    }

    printk(LOG_INFO, "[GPT PARSE] Reading GPT Primary Header at LBA 1...\n");
    if (!raw_drive_read(drive, 1, 1, sector_buffer_virt)) {
        printk(LOG_ERROR, "[GPT PARSE] I/O error reading LBA 1 header.\n");
        kfree(sector_buffer_virt);
        return table;
    }

    gpt_header_t* header = (gpt_header_t*)sector_buffer_virt;
    printk(LOG_INFO, "[GPT PARSE] Header Sig: 0x%llx | Revision: 0x%x | Header Size: %u bytes\n",
           header->signature, header->revision, header->header_size);

    if (header->signature != GPT_SIGNATURE) {
        printk(LOG_ERROR, "[GPT ERROR] Signature validation failed (expected 0x%llx).\n", GPT_SIGNATURE);
        kfree(sector_buffer_virt);
        return table;
    }

    uint64_t entry_lba = header->partition_entry_lba;
    uint32_t num_entries = header->num_partition_entries;
    uint32_t entry_size = header->size_of_partition_entry;
    uint32_t total_bytes = num_entries * entry_size;
    uint32_t sectors_to_read = (total_bytes + sector_size - 1) / sector_size;

    printk(LOG_INFO, "[GPT PARSE] Array LBA: %llu | Entries: %u | Entry Size: %u bytes | Read Sectors: %u\n",
           entry_lba, num_entries, entry_size, sectors_to_read);

    void* array_virt = kmalloc(sectors_to_read * sector_size);
    if (!array_virt) {
        printk(LOG_ERROR, "[GPT PARSE] Failed to allocate partition array buffer.\n");
        kfree(sector_buffer_virt);
        return table;
    }

    printk(LOG_INFO, "[GPT PARSE] Reading Partition Array at LBA %llu...\n", entry_lba);
    if (!raw_drive_read(drive, entry_lba, (uint16_t)sectors_to_read, array_virt)) {
        printk(LOG_ERROR, "[GPT PARSE] I/O error reading partition entry array.\n");
        kfree(array_virt);
        kfree(sector_buffer_virt);
        return table;
    }

    uint8_t* byte_ptr = (uint8_t*)array_virt;
    for (uint32_t i = 0; i < num_entries; i++) {
        gpt_entry_t* entry = (gpt_entry_t*)(byte_ptr + (i * entry_size));
        const uint8_t* guid_bytes = (const uint8_t*)&entry->partition_type_guid;

        // Check if GUID is all zeros
        int is_zero_guid = 1;
        for (int k = 0; k < 16; k++) {
            if (guid_bytes[k] != 0) {
                is_zero_guid = 0;
                break;
            }
        }

        // Log non-zero entries or entries with valid LBAs
        if (!is_zero_guid || entry->starting_lba != 0 || entry->ending_lba != 0) {
            printk(LOG_INFO, "[GPT PARSE] Slot %03u | GUID: %02x%02x%02x%02x-%02x%02x-%02x%02x... | Start LBA: %llu | End LBA: %llu\n",
                   i, guid_bytes[0], guid_bytes[1], guid_bytes[2], guid_bytes[3],
                   guid_bytes[4], guid_bytes[5], guid_bytes[6], guid_bytes[7],
                   entry->starting_lba, entry->ending_lba);
        }

        // Filter 1: Zero GUID
        if (is_zero_guid) {
            continue;
        }

        // Filter 2: Invalid/Inverted LBAs
        if (entry->starting_lba == 0 && entry->ending_lba == 0) {
            printk(LOG_WARNING, "[GPT PARSE] Slot %03u ignored: Starting and Ending LBA are 0.\n", i);
            continue;
        }
        if (entry->starting_lba > entry->ending_lba) {
            printk(LOG_WARNING, "[GPT PARSE] Slot %03u ignored: Starting LBA (%llu) > Ending LBA (%llu).\n",
                   i, entry->starting_lba, entry->ending_lba);
            continue;
        }

        uint64_t sector_count = (entry->ending_lba - entry->starting_lba) + 1;

        char ascii_name[36];
        int c;
        for (c = 0; c < 35; c++) {
            if (entry->partition_name[c] == 0) break;
            ascii_name[c] = (char)entry->partition_name[c];
        }
        ascii_name[c] = '\0';

        printk(LOG_INFO, "[GPT PARSE] -> Valid Partition Found! Slot %03u | Name: '%s' | Sectors: %llu\n",
               i, ascii_name, sector_count);

        if (volume_count < MAX_VOLUMES) {
            int current_vol_idx = volume_count;
            volume_register(*drive, entry->starting_lba, sector_count, ascii_name);

            if (table.count < MAX_VOLUMES) {
                table.partitions[table.count] = &system_volumes[current_vol_idx];
                table.count++;
            }
        } else {
            printk(LOG_WARNING, "[GPT PARSE] MAX_VOLUMES limit reached. Skipping registration for slot %03u.\n", i);
        }
    }

    printk(LOG_INFO, "[GPT PARSE] Parsing completed. Total registered valid partitions: %d\n", table.count);

    kfree(array_virt);
    kfree(sector_buffer_virt);
    return table;
}

/**
 * Formats a drive with a clean, blank GPT layout (Protective MBR + Header + 128 Empty Slots).
 */
partition_table_t gpt_format_disk(generic_drive_t* drive) {
    partition_table_t result;

    if (!drive) {
        result.count = 0;
        return result;
    };

    uint32_t sector_size = (drive->sector_size > 0) ? drive->sector_size : 512;
    uint32_t array_sectors = (128 * 128 + sector_size - 1) / sector_size;
    uint32_t alloc_sectors = 2 + array_sectors; // MBR + Header + Partition Array

    if (drive->type == DRIVE_TYPE_AHCI) {
        printk(LOG_DEBUG, "[GPT FORMAT] Formatting AHCI drive on port %d...\n", drive->ahci_drive->port_number);
    } else {
        printk(LOG_DEBUG, "[GPT FORMAT] Formatting NVMe drive ID %u...\n", drive->nvme.nvme_id);
    }

    void* scratch_virt = kmalloc(sector_size * alloc_sectors);
    if (!scratch_virt) {
        result.count = 0;
        return result;
    }
    memset(scratch_virt, 0, sector_size * alloc_sectors);

    // 1. Create Protective MBR (LBA 0)
    uint8_t* mbr = (uint8_t*)scratch_virt;
    mbr[510] = 0x55; mbr[511] = 0xAA; // MBR Signature
    mbr[446] = 0x00;                  // Boot indicator
    mbr[447] = 0x00; mbr[448] = 0x02; mbr[449] = 0x00; // Starting CHS
    mbr[450] = 0xEE;                  // OS Type: GPT Protective MBR
    mbr[451] = 0xFF; mbr[452] = 0xFF; mbr[453] = 0xFF; // Ending CHS
    mbr[454] = 0x01; mbr[455] = 0x00; mbr[456] = 0x00; mbr[457] = 0x00; // Starting LBA (1)
    uint32_t max_sectors = (drive->total_sectors > 0xFFFFFFFF) ? 0xFFFFFFFF : (uint32_t)drive->total_sectors - 1;
    memcpy(&mbr[458], &max_sectors, 4);

    // Write Protective MBR
    if (!raw_drive_write(drive, 0, 1, scratch_virt)) {
        kfree(scratch_virt);
        result.count = 0;
        return result;
    }

    // 2. Set up Blank Partition Entry Array (128 Entries * 128 Bytes)
    memset(scratch_virt, 0, sector_size * array_sectors);
    uint32_t array_crc = crc32(scratch_virt, 128 * 128);

    // Write empty array to Primary Entry LBA (2) and Backup Entry LBA
    if (!raw_drive_write(drive, 2, (uint16_t)array_sectors, scratch_virt) ||
        !raw_drive_write(drive, drive->total_sectors - 1 - array_sectors, (uint16_t)array_sectors, scratch_virt)) {
        kfree(scratch_virt);
        result.count = 0;
        return result;
    }

    // 3. Set up Primary GPT Header (LBA 1)
    gpt_header_t* primary_hdr = (gpt_header_t*)scratch_virt;
    memset(primary_hdr, 0, sector_size);
    primary_hdr->signature = GPT_SIGNATURE;
    primary_hdr->revision = 0x00010000; // Version 1.0
    primary_hdr->header_size = 92;
    primary_hdr->my_lba = 1;
    primary_hdr->alternate_lba = drive->total_sectors - 1;
    primary_hdr->first_usable_lba = 2 + array_sectors;
    primary_hdr->last_usable_lba = drive->total_sectors - 2 - array_sectors;
    primary_hdr->partition_entry_lba = 2;
    primary_hdr->num_partition_entries = 128;
    primary_hdr->size_of_partition_entry = 128;
    primary_hdr->partition_array_crc32 = array_crc;
    primary_hdr->header_crc32 = crc32(primary_hdr, 92);

    if (!raw_drive_write(drive, 1, 1, scratch_virt)) {
        kfree(scratch_virt);
        result.count = 0;
        return result;
    }

    // 4. Set up Backup GPT Header (Last LBA)
    gpt_header_t* backup_hdr = (gpt_header_t*)scratch_virt;
    backup_hdr->header_crc32 = 0; // Reset before recalculation
    backup_hdr->my_lba = drive->total_sectors - 1;
    backup_hdr->alternate_lba = 1;
    backup_hdr->partition_entry_lba = drive->total_sectors - 1 - array_sectors;
    backup_hdr->header_crc32 = crc32(backup_hdr, 92);

    if (!raw_drive_write(drive, drive->total_sectors - 1, 1, scratch_virt)) {
        kfree(scratch_virt);
        result.count = 0;
        return result;
    }

    kfree(scratch_virt);
    return gpt_parse_partitions(drive); // Reload empty table into memory
}

/**
 * Creates a partition inside a blank slot using a specified starting sector block count.
 */
int gpt_create_partition(generic_drive_t* drive, const char* name, uint64_t sector_count) {
    if (!drive || sector_count == 0) return 0;

    uint32_t sector_size = (drive->sector_size > 0) ? drive->sector_size : 512;
    uint32_t array_sectors = (128 * 128 + sector_size - 1) / sector_size;

    void* hdr_buf_virt = kmalloc(sector_size);
    void* array_buf_virt = kmalloc(array_sectors * sector_size);
    if (!hdr_buf_virt || !array_buf_virt) {
        if (hdr_buf_virt) kfree(hdr_buf_virt);
        if (array_buf_virt) kfree(array_buf_virt);
        return 0;
    }

    // Read existing details
    if (!raw_drive_read(drive, 1, 1, hdr_buf_virt) ||
        !raw_drive_read(drive, 2, (uint16_t)array_sectors, array_buf_virt)) {
        kfree(hdr_buf_virt); kfree(array_buf_virt);
        return 0;
    }

    gpt_header_t* header = (gpt_header_t*)hdr_buf_virt;
    gpt_entry_t* entries = (gpt_entry_t*)array_buf_virt;

    // Find closest safe configuration space starting past usable bounds
    uint64_t dynamic_start_lba = header->first_usable_lba;
    int target_slot = -1;

    for (uint32_t i = 0; i < header->num_partition_entries; i++) {
        uint64_t* guid = (uint64_t*)&entries[i].partition_type_guid;
        if (guid[0] != 0 || guid[1] != 0) {
            // Space optimization bump past existing blocks
            if (entries[i].ending_lba >= dynamic_start_lba) {
                dynamic_start_lba = entries[i].ending_lba + 1;
            }
        } else if (target_slot == -1) {
            target_slot = i; // First blank structural metadata line found
        }
    }

    if (target_slot == -1 || (dynamic_start_lba + sector_count - 1) > header->last_usable_lba) {
        printk(LOG_DEBUG, "[GPT CREATE ERROR] Space constraints or slot limitation limits reached.\n");
        kfree(hdr_buf_virt); kfree(array_buf_virt);
        return 0;
    }

    // Populate slot fields
    gpt_entry_t* new_entry = &entries[target_slot];
    // Generic Basic Data Partition GUID: EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
    // Valid GPT Basic Data Partition GUID: EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
    new_entry->partition_type_guid.data1 = 0xEBD0A0A2;
    new_entry->partition_type_guid.data2 = 0xB9E5;
    new_entry->partition_type_guid.data3 = 0x4433;
    new_entry->partition_type_guid.data4[0] = 0x87;
    new_entry->partition_type_guid.data4[1] = 0xC0;
    new_entry->partition_type_guid.data4[2] = 0x68;
    new_entry->partition_type_guid.data4[3] = 0xB6;
    new_entry->partition_type_guid.data4[4] = 0xB7;
    new_entry->partition_type_guid.data4[5] = 0x26;
    new_entry->partition_type_guid.data4[6] = 0x99;
    new_entry->partition_type_guid.data4[7] = 0xC7;
    new_entry->unique_partition_guid.data1 = target_slot + 0xABC123;
    new_entry->starting_lba = dynamic_start_lba;
    new_entry->ending_lba = dynamic_start_lba + sector_count - 1;
    new_entry->attributes = 0;

    // Convert ASCII string parameter into UTF-16 array structure
    memset(new_entry->partition_name, 0, sizeof(new_entry->partition_name));
    for (int c = 0; c < 35 && name[c] != '\0'; c++) {
        new_entry->partition_name[c] = (uint16_t)name[c];
    }

    // Recalculate global validation checksum metrics
    uint32_t array_crc = crc32(array_buf_virt, 128 * 128);
    header->partition_array_crc32 = array_crc;
    header->header_crc32 = 0;
    header->header_crc32 = crc32(header, 92);

    // Commit changes to Primary Table
    if (!raw_drive_write(drive, 1, 1, hdr_buf_virt) ||
        !raw_drive_write(drive, 2, (uint16_t)array_sectors, array_buf_virt)) {
        kfree(hdr_buf_virt); kfree(array_buf_virt);
        return 0;
    }

    // Commit changes to Backup Table (Disk End)
    header->my_lba = drive->total_sectors - 1;
    header->alternate_lba = 1;
    header->partition_entry_lba = drive->total_sectors - 1 - array_sectors;
    header->header_crc32 = 0;
    header->header_crc32 = crc32(header, 92);

    if (!raw_drive_write(drive, drive->total_sectors - 1 - array_sectors, (uint16_t)array_sectors, array_buf_virt) ||
        !raw_drive_write(drive, drive->total_sectors - 1, 1, hdr_buf_virt)) {
        kfree(hdr_buf_virt); kfree(array_buf_virt);
        return 0;
    }

    kfree(hdr_buf_virt); kfree(array_buf_virt);
    gpt_parse_partitions(drive); // Sync memory tables
    return 1;
}

/**
 * Deletes a partition by targeting its array index slot.
 */
int gpt_delete_partition(generic_drive_t* drive, int index_slot) {
    if (!drive || index_slot < 0 || index_slot >= 128) return 0;

    uint32_t sector_size = (drive->sector_size > 0) ? drive->sector_size : 512;
    uint32_t array_sectors = (128 * 128 + sector_size - 1) / sector_size;

    void* hdr_buf_virt = kmalloc(sector_size);
    void* array_buf_virt = kmalloc(array_sectors * sector_size);
    if (!hdr_buf_virt || !array_buf_virt) {
        if (hdr_buf_virt) kfree(hdr_buf_virt);
        if (array_buf_virt) kfree(array_buf_virt);
        return 0;
    }

    if (!raw_drive_read(drive, 1, 1, hdr_buf_virt) ||
        !raw_drive_read(drive, 2, (uint16_t)array_sectors, array_buf_virt)) {
        kfree(hdr_buf_virt); kfree(array_buf_virt);
        return 0;
    }

    gpt_header_t* header = (gpt_header_t*)hdr_buf_virt;
    gpt_entry_t* entries = (gpt_entry_t*)array_buf_virt;

    // Zero out the targeted entry data fields entirely
    memset(&entries[index_slot], 0, sizeof(gpt_entry_t));

    // Recalculate checksum hashes
    uint32_t array_crc = crc32(array_buf_virt, 128 * 128);
    header->partition_array_crc32 = array_crc;
    header->header_crc32 = 0;
    header->header_crc32 = crc32(header, 92);

    // Write back primary structures
    if (!raw_drive_write(drive, 1, 1, hdr_buf_virt) ||
        !raw_drive_write(drive, 2, (uint16_t)array_sectors, array_buf_virt)) {
        kfree(hdr_buf_virt); kfree(array_buf_virt);
        return 0;
    }

    // Write back backup elements
    header->my_lba = drive->total_sectors - 1;
    header->alternate_lba = 1;
    header->partition_entry_lba = drive->total_sectors - 1 - array_sectors;
    header->header_crc32 = 0;
    header->header_crc32 = crc32(header, 92);

    if (!raw_drive_write(drive, drive->total_sectors - 1 - array_sectors, (uint16_t)array_sectors, array_buf_virt) ||
        !raw_drive_write(drive, drive->total_sectors - 1, 1, hdr_buf_virt)) {
        kfree(hdr_buf_virt); kfree(array_buf_virt);
        return 0;
    }

    kfree(hdr_buf_virt); kfree(array_buf_virt);
    gpt_parse_partitions(drive); // Refresh local operating structures
    return 1;
}