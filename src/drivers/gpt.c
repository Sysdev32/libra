#include <drivers/gpt.h>
#include <drivers/fb.h>
#include <drivers/alloc.h> // kmalloc and kfree
#include <string.h>

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

void volume_register(ahci_device_t* drive, uint64_t start_lba, uint64_t total_sectors, const char* name) {
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

int volume_read_sectors(volume_t* vol, uint64_t relative_lba, uint16_t count, uint64_t buf_phys) {
    if (!vol || !vol->is_valid) return 0;
    if (relative_lba + count > vol->total_sectors) {
        printk(LOG_DEBUG, "[VOLUME IO ERROR] Attempted to read past partition boundaries!\n");
        return 0;
    }
    uint64_t absolute_lba = vol->start_lba + relative_lba;
    return ahci_read_sectors(vol->drive, absolute_lba, count, buf_phys);
}

int volume_write_sectors(volume_t* vol, uint64_t relative_lba, uint16_t count, uint64_t buf_phys) {
    if (!vol || !vol->is_valid) return 0;
    if (relative_lba + count > vol->total_sectors) {
        printk(LOG_DEBUG, "[VOLUME IO ERROR] Attempted to write past partition boundaries!\n");
        return 0;
    }
    uint64_t absolute_lba = vol->start_lba + relative_lba;
    return ahci_write_sectors(vol->drive, absolute_lba, count, buf_phys); // Fixed: changed read to write
}

void gpt_parse_partitions(ahci_device_t* drive) {
    if (!drive || !drive->is_initialized) return;

    // Reset volume table for this drive parse
    volume_count = 0;
    memset(system_volumes, 0, sizeof(system_volumes));

    void* sector_buffer_virt = kmalloc(512);
    if (!sector_buffer_virt) return;

    uint64_t sector_buffer_phys = (uint64_t)sector_buffer_virt - (uint64_t)HHDM_OFFSET;

    if (!ahci_read_sectors(drive, 1, 1, sector_buffer_phys)) {
        kfree(sector_buffer_virt);
        return;
    }

    gpt_header_t* header = (gpt_header_t*)sector_buffer_virt;
    if (header->signature != GPT_SIGNATURE) {
        printk(LOG_DEBUG, "[GPT ERROR] Signature validation failed.\n");
        kfree(sector_buffer_virt);
        return;
    }

    uint64_t entry_lba = header->partition_entry_lba;
    uint32_t num_entries = header->num_partition_entries;
    uint32_t entry_size = header->size_of_partition_entry;
    uint32_t total_bytes = num_entries * entry_size;
    uint32_t sectors_to_read = (total_bytes + 511) / 512;

    void* array_virt = kmalloc(sectors_to_read * 512); 
    if (!array_virt) {
        kfree(sector_buffer_virt);
        return;
    }

    uint64_t array_phys = (uint64_t)array_virt - (uint64_t)HHDM_OFFSET;
    if (!ahci_read_sectors(drive, entry_lba, sectors_to_read, array_phys)) {
        kfree(array_virt);
        kfree(sector_buffer_virt);
        return;
    }

    uint8_t* byte_ptr = (uint8_t*)array_virt;
    for (uint32_t i = 0; i < num_entries; i++) {
        gpt_entry_t* entry = (gpt_entry_t*)(byte_ptr + (i * entry_size));
        uint64_t* guid_check = (uint64_t*)&entry->partition_type_guid;
        
        if (guid_check[0] == 0 && guid_check[1] == 0) continue;

        uint64_t sector_count = (entry->ending_lba - entry->starting_lba) + 1;
        
        char ascii_name[36];
        int c;
        for(c = 0; c < 36; c++) {
            if(entry->partition_name[c] == 0) break;
            ascii_name[c] = (char)entry->partition_name[c];
        }
        ascii_name[c] = '\0';

        volume_register(drive, entry->starting_lba, sector_count, ascii_name);
    }

    kfree(array_virt);
    kfree(sector_buffer_virt);
}

/**
 * Formats a drive with a clean, blank GPT layout (Protective MBR + Header + 128 Empty Slots).
 */
int gpt_format_disk(ahci_device_t* drive) {
    if (!drive || !drive->is_initialized) return 0;

    printk(LOG_DEBUG, "[GPT FORMAT] Formatting drive on port %d...\n", drive->port_number);

    void* scratch_virt = kmalloc(512 * 34); // Allocate enough for MBR, Header, and 32 sectors of entries
    if (!scratch_virt) return 0;
    memset(scratch_virt, 0, 512 * 34);
    uint64_t scratch_phys = (uint64_t)scratch_virt - (uint64_t)HHDM_OFFSET;

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
    if (!ahci_write_sectors(drive, 0, 1, scratch_phys)) {
        kfree(scratch_virt);
        return 0;
    }

    // 2. Set up Blank Partition Entry Array (128 Entries * 128 Bytes = 32 Sectors)
    memset(scratch_virt, 0, 512 * 32);
    uint32_t array_crc = crc32(scratch_virt, 128 * 128);

    // Write empty array to Primary Entry LBA (2 to 33) and Backup Entry LBA
    if (!ahci_write_sectors(drive, 2, 32, scratch_phys) ||
        !ahci_write_sectors(drive, drive->total_sectors - 33, 32, scratch_phys)) {
        kfree(scratch_virt);
        return 0;
    }

    // 3. Set up Primary GPT Header (LBA 1)
    gpt_header_t* primary_hdr = (gpt_header_t*)scratch_virt;
    memset(primary_hdr, 0, 512);
    primary_hdr->signature = GPT_SIGNATURE;
    primary_hdr->revision = 0x00010000; // Version 1.0
    primary_hdr->header_size = 92;
    primary_hdr->my_lba = 1;
    primary_hdr->alternate_lba = drive->total_sectors - 1;
    primary_hdr->first_usable_lba = 34;
    primary_hdr->last_usable_lba = drive->total_sectors - 34;
    primary_hdr->partition_entry_lba = 2;
    primary_hdr->num_partition_entries = 128;
    primary_hdr->size_of_partition_entry = 128;
    primary_hdr->partition_array_crc32 = array_crc;
    primary_hdr->header_crc32 = crc32(primary_hdr, 92);

    if (!ahci_write_sectors(drive, 1, 1, scratch_phys)) {
        kfree(scratch_virt);
        return 0;
    }

    // 4. Set up Backup GPT Header (Last LBA)
    gpt_header_t* backup_hdr = (gpt_header_t*)scratch_virt;
    backup_hdr->header_crc32 = 0; // Reset before recalculation
    backup_hdr->my_lba = drive->total_sectors - 1;
    backup_hdr->alternate_lba = 1;
    backup_hdr->partition_entry_lba = drive->total_sectors - 33;
    backup_hdr->header_crc32 = crc32(backup_hdr, 92);

    if (!ahci_write_sectors(drive, drive->total_sectors - 1, 1, scratch_phys)) {
        kfree(scratch_virt);
        return 0;
    }

    kfree(scratch_virt);
    gpt_parse_partitions(drive); // Reload empty table into memory
    return 1;
}

/**
 * Creates a partition inside a blank slot using a specified starting sector block count.
 */
int gpt_create_partition(ahci_device_t* drive, const char* name, uint64_t sector_count) {
    if (!drive || !drive->is_initialized || sector_count == 0) return 0;

    void* hdr_buf_virt = kmalloc(512);
    void* array_buf_virt = kmalloc(128 * 128);
    if (!hdr_buf_virt || !array_buf_virt) {
        if (hdr_buf_virt) kfree(hdr_buf_virt);
        if (array_buf_virt) kfree(array_buf_virt);
        return 0;
    }

    uint64_t hdr_buf_phys = (uint64_t)hdr_buf_virt - (uint64_t)HHDM_OFFSET;
    uint64_t array_buf_phys = (uint64_t)array_buf_virt - (uint64_t)HHDM_OFFSET;

    // Read existing details
    if (!ahci_read_sectors(drive, 1, 1, hdr_buf_phys) ||
        !ahci_read_sectors(drive, 2, 32, array_buf_phys)) {
        kfree(hdr_buf_virt); kfree(array_buf_virt);
        return 0;
    }

    gpt_header_t* header = (gpt_header_t*)hdr_buf_virt;
    gpt_entry_t* entries = (gpt_entry_t*)array_buf_virt;

    // Find closest safe configuration space starting past 34
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
    new_entry->partition_type_guid.data1 = 0x287C068B6; // Simplified representation
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

    // Commit changes to Primary Table (LBAs 1-33)
    if (!ahci_write_sectors(drive, 1, 1, hdr_buf_phys) ||
        !ahci_write_sectors(drive, 2, 32, array_buf_phys)) {
        kfree(hdr_buf_virt); kfree(array_buf_virt);
        return 0;
    }

    // Commit changes to Backup Table (Disk End)
    header->my_lba = drive->total_sectors - 1;
    header->alternate_lba = 1;
    header->partition_entry_lba = drive->total_sectors - 33;
    header->header_crc32 = 0;
    header->header_crc32 = crc32(header, 92);

    if (!ahci_write_sectors(drive, drive->total_sectors - 33, 32, array_buf_phys) ||
        !ahci_write_sectors(drive, drive->total_sectors - 1, 1, hdr_buf_phys)) {
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
int gpt_delete_partition(ahci_device_t* drive, int index_slot) {
    if (!drive || !drive->is_initialized || index_slot < 0 || index_slot >= 128) return 0;

    void* hdr_buf_virt = kmalloc(512);
    void* array_buf_virt = kmalloc(128 * 128);
    if (!hdr_buf_virt || !array_buf_virt) {
        if (hdr_buf_virt) kfree(hdr_buf_virt);
        if (array_buf_virt) kfree(array_buf_virt);
        return 0;
    }

    uint64_t hdr_buf_phys = (uint64_t)hdr_buf_virt - (uint64_t)HHDM_OFFSET;
    uint64_t array_buf_phys = (uint64_t)array_buf_virt - (uint64_t)HHDM_OFFSET;

    if (!ahci_read_sectors(drive, 1, 1, hdr_buf_phys) ||
        !ahci_read_sectors(drive, 2, 32, array_buf_phys)) {
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
    if (!ahci_write_sectors(drive, 1, 1, hdr_buf_phys) ||
        !ahci_write_sectors(drive, 2, 32, array_buf_phys)) {
        kfree(hdr_buf_virt); kfree(array_buf_virt);
        return 0;
    }

    // Write back backup elements
    header->my_lba = drive->total_sectors - 1;
    header->alternate_lba = 1;
    header->partition_entry_lba = drive->total_sectors - 33;
    header->header_crc32 = 0;
    header->header_crc32 = crc32(header, 92);

    if (!ahci_write_sectors(drive, drive->total_sectors - 33, 32, array_buf_phys) ||
        !ahci_write_sectors(drive, drive->total_sectors - 1, 1, hdr_buf_phys)) {
        kfree(hdr_buf_virt); kfree(array_buf_virt);
        return 0;
    }

    kfree(hdr_buf_virt); kfree(array_buf_virt);
    gpt_parse_partitions(drive); // Refresh local operating structures
    return 1;
}