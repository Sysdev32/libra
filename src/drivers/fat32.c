#include <drivers/fat32.h>
#include <drivers/alloc.h>
#include <drivers/fb.h>
#include <string.h>

#define FAT32_EOF         0x0FFFFFF8
#define FAT32_FREE_ENTRY  0xE5
#define FAT32_END_OF_DIR  0x00

/* --- INTERNAL LFN HELPER FUNCTIONS --- */

static uint32_t cluster_to_sector(fat32_fs_t* fs, uint32_t cluster) {
    return fs->data_start_sector + ((cluster - 2) * fs->sectors_per_cluster);
}

static uint8_t fat32_checksum_sfn(const char* sfn) {
    uint8_t sum = 0;
    for (int i = 11; i > 0; i--) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *sfn++;
    }
    return sum;
}

static void fat32_generate_sfn(const char* lfn, char* out_sfn) {
    memset(out_sfn, ' ', 11);
    int i = 0, j = 0;
    while (lfn[i] && lfn[i] != '.' && j < 6) {
        char c = lfn[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != ' ' && c != '.') out_sfn[j++] = c;
    }
    out_sfn[6] = '~';
    out_sfn[7] = '1';
    const char* ext = strrchr(lfn, '.');
    if (ext && strlen(ext) > 1) {
        int ext_idx = 8;
        ext++;
        while (*ext && ext_idx < 11) {
            char c = *ext++;
            if (c >= 'a' && c <= 'z') c -= 32;
            out_sfn[ext_idx++] = c;
        }
    }
}

/**
 * Extracts UTF-16 characters out of an LFN slot and appends them as ASCII into a target string buffer.
 */
static void fat32_extract_lfn_chars(fat32_lfn_t* lfn, char* buffer) {
    int idx = ((lfn->sequence_number & 0x1F) - 1) * 13;
    
    for (int i = 0; i < 5; i++) {
        uint16_t c = lfn->name_characters1[i];
        if (c == 0x0000 || c == 0xFFFF) return;
        buffer[idx++] = (char)(c & 0xFF);
    }
    for (int i = 0; i < 6; i++) {
        uint16_t c = lfn->name_characters2[i];
        if (c == 0x0000 || c == 0xFFFF) return;
        buffer[idx++] = (char)(c & 0xFF);
    }
    for (int i = 0; i < 2; i++) {
        uint16_t c = lfn->name_characters3[i];
        if (c == 0x0000 || c == 0xFFFF) return;
        buffer[idx++] = (char)(c & 0xFF);
    }
}

/* --- CORE INITIALIZATION & LOW-LEVEL FAT MECHANICS --- */

int fat32_init(volume_t* vol, fat32_fs_t* fs) {
    if (!vol || !vol->is_valid) return 0;
    
    void* bpb_virt = kmalloc(512);
    if (!bpb_virt) return 0;
    uint64_t bpb_phys = (uint64_t)bpb_virt - (uint64_t)HHDM_OFFSET;

    if (!volume_read_sectors(vol, 0, 1, bpb_phys)) {
        kfree(bpb_virt); return 0;
    }

    fat32_bpb_t* bpb = (fat32_bpb_t*)bpb_virt;
    if (bpb->bytes_per_sector != 512) { kfree(bpb_virt); return 0; }

    fs->vol = vol;
    fs->sectors_per_cluster = bpb->sectors_per_cluster;
    fs->root_cluster = bpb->root_cluster;
    fs->fat_start_sector = bpb->reserved_sector_count;
    fs->data_start_sector = fs->fat_start_sector + (bpb->num_fats * bpb->sectors_per_fat_32);

    kfree(bpb_virt);
    printk(LOG_DEBUG, "[FAT32 LFN] Filesystem mounted successfully.\n");
    return 1;
}

uint32_t fat32_get_next_cluster(fat32_fs_t* fs, uint32_t current_cluster) {
    uint32_t fat_offset = current_cluster * 4;
    uint32_t fat_sector = fs->fat_start_sector + (fat_offset / 512);
    uint32_t ent_offset = fat_offset % 512;

    void* sector_buf = kmalloc(512);
    if (!sector_buf) return FAT32_EOF;
    uint64_t phys_buf = (uint64_t)sector_buf - (uint64_t)HHDM_OFFSET;

    if (!volume_read_sectors(fs->vol, fat_sector, 1, phys_buf)) {
        kfree(sector_buf); return FAT32_EOF;
    }

    uint32_t next_cluster = *(uint32_t*)((uint8_t*)sector_buf + ent_offset) & 0x0FFFFFFF;
    kfree(sector_buf);
    return next_cluster;
}

int fat32_set_cluster_value(fat32_fs_t* fs, uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fs->fat_start_sector + (fat_offset / 512);
    uint32_t ent_offset = fat_offset % 512;

    void* sector_buf = kmalloc(512);
    if (!sector_buf) return 0;
    uint64_t phys_buf = (uint64_t)sector_buf - (uint64_t)HHDM_OFFSET;

    if (!volume_read_sectors(fs->vol, fat_sector, 1, phys_buf)) {
        kfree(sector_buf); return 0;
    }

    uint32_t* entry_ptr = (uint32_t*)((uint8_t*)sector_buf + ent_offset);
    *entry_ptr = (*entry_ptr & 0xF0000000) | (value & 0x0FFFFFFF);

    if (!volume_write_sectors(fs->vol, fat_sector, 1, phys_buf)) {
        kfree(sector_buf); return 0;
    }
    kfree(sector_buf);
    return 1;
}

uint32_t fat32_allocate_cluster(fat32_fs_t* fs) {
    void* sector_buf = kmalloc(512);
    if (!sector_buf) return 0;
    uint64_t phys_buf = (uint64_t)sector_buf - (uint64_t)HHDM_OFFSET;

    for (uint32_t s = 0; s < 128; s++) {
        uint32_t current_fat_sec = fs->fat_start_sector + s;
        if (!volume_read_sectors(fs->vol, current_fat_sec, 1, phys_buf)) break;

        uint32_t* entries = (uint32_t*)sector_buf;
        for (int i = 0; i < 128; i++) {
            if ((entries[i] & 0x0FFFFFFF) == 0) {
                uint32_t found_cluster = (s * 128) + i;
                if (found_cluster < 2) continue;
                fat32_set_cluster_value(fs, found_cluster, FAT32_EOF);
                kfree(sector_buf);
                return found_cluster;
            }
        }
    }
    kfree(sector_buf);
    return 0;
}

/* --- REWRITTEN VFS VIRTUAL LAYER FOR LFN PROCESSING --- */

/**
 * Scans a directory parsing consecutive long file entries to discover matching items.
 */
uint32_t fat32_find_object_lfn(fat32_fs_t* fs, uint32_t dir_cluster, const char* target_name, fat32_entry_t* out_entry) {
    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    void* dir_buf = kmalloc(cluster_bytes);
    if (!dir_buf) return 0;
    uint64_t phys_buf = (uint64_t)dir_buf - (uint64_t)HHDM_OFFSET;

    char lfn_accumulator[256];
    memset(lfn_accumulator, 0, 256);

    uint32_t curr_cluster = dir_cluster;
    while (curr_cluster >= 2 && curr_cluster < 0x0FFFFFF8) {
        uint32_t sector = cluster_to_sector(fs, curr_cluster);
        if (!volume_read_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_buf)) break;

        fat32_entry_t* entries = (fat32_entry_t*)dir_buf;
        uint32_t max_slots = cluster_bytes / 32;

        for (uint32_t i = 0; i < max_slots; i++) {
            if (entries[i].name[0] == FAT32_END_OF_DIR) { kfree(dir_buf); return 0; }
            if (entries[i].name[0] == (char)FAT32_FREE_ENTRY) {
                memset(lfn_accumulator, 0, 256); continue;
            }

            if (entries[i].attr == FAT_ATTR_LONG_NAME) {
                fat32_lfn_t* lfn = (fat32_lfn_t*)&entries[i];
                fat32_extract_lfn_chars(lfn, lfn_accumulator);
            } else {
                // SFN standard fallback comparison path
                char standard_name[13];
                memset(standard_name, 0, 13);
                int p = 0;
                for (int c = 0; c < 8; c++) if (entries[i].name[c] != ' ') standard_name[p++] = entries[i].name[c];
                if (entries[i].name[8] != ' ') {
                    standard_name[p++] = '.';
                    for (int c = 8; c < 11; c++) if (entries[i].name[c] != ' ') standard_name[p++] = entries[i].name[c];
                }

                // Match against either calculated LFN string or SFN raw fallback character line
                if (strcmp(lfn_accumulator, target_name) == 0 || strcmp(standard_name, target_name) == 0) {
                    if (out_entry) *out_entry = entries[i];
                    uint32_t res_cluster = ((uint32_t)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
                    kfree(dir_buf);
                    return res_cluster;
                }
                memset(lfn_accumulator, 0, 256); // Clean slate for next logical entry chain
            }
        }
        curr_cluster = fat32_get_next_cluster(fs, curr_cluster);
    }
    kfree(dir_buf);
    return 0;
}

void fat32_list_directory_lfn(fat32_fs_t* fs, uint32_t dir_cluster) {
    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    void* dir_buf = kmalloc(cluster_bytes);
    if (!dir_buf) return;
    uint64_t phys_buf = (uint64_t)dir_buf - (uint64_t)HHDM_OFFSET;

    char lfn_accumulator[256];
    memset(lfn_accumulator, 0, 256);

    uint32_t curr_cluster = dir_cluster;
    printk(LOG_DEBUG, "=== DIRECTORY LISTING VIA FULL LFN ===\n");
    
    while (curr_cluster >= 2 && curr_cluster < 0x0FFFFFF8) {
        uint32_t sector = cluster_to_sector(fs, curr_cluster);
        if (!volume_read_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_buf)) break;

        fat32_entry_t* entries = (fat32_entry_t*)dir_buf;
        uint32_t max_slots = cluster_bytes / 32;

        for (uint32_t i = 0; i < max_slots; i++) {
            if (entries[i].name[0] == FAT32_END_OF_DIR) { kfree(dir_buf); return; }
            if (entries[i].name[0] == (char)FAT32_FREE_ENTRY) { memset(lfn_accumulator, 0, 256); continue; }

            if (entries[i].attr == FAT_ATTR_LONG_NAME) {
                fat32_lfn_t* lfn = (fat32_lfn_t*)&entries[i];
                fat32_extract_lfn_chars(lfn, lfn_accumulator);
            } else {
                if (entries[i].attr & FAT_ATTR_VOLUME_ID) continue;

                if (strlen(lfn_accumulator) > 0) {
                    printk(LOG_DEBUG, " * %s [%s] Size: %u bytes\n", lfn_accumulator, (entries[i].attr & FAT_ATTR_DIRECTORY) ? "DIR" : "FILE", entries[i].file_size);
                } else {
                    char sfn_print[12]; memset(sfn_print, 0, 12); memcpy(sfn_print, entries[i].name, 11);
                    printk(LOG_DEBUG, " * %s [%s] Size: %u bytes\n", sfn_print, (entries[i].attr & FAT_ATTR_DIRECTORY) ? "DIR" : "FILE", entries[i].file_size);
                }
                memset(lfn_accumulator, 0, 256);
            }
        }
        curr_cluster = fat32_get_next_cluster(fs, curr_cluster);
    }
    kfree(dir_buf);
}

int fat32_read_file_lfn(fat32_fs_t* fs, uint32_t dir_cluster, const char* filename, uint8_t* out_buffer, uint32_t max_bytes) {
    fat32_entry_t object_meta;
    uint32_t start_cluster = fat32_find_object_lfn(fs, dir_cluster, filename, &object_meta);
    if (start_cluster == 0 || (object_meta.attr & FAT_ATTR_DIRECTORY)) return 0;

    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    void* cluster_buf = kmalloc(cluster_bytes);
    if (!cluster_buf) return 0;
    uint64_t phys_buf = (uint64_t)cluster_buf - (uint64_t)HHDM_OFFSET;

    uint32_t current_cluster = start_cluster;
    uint32_t bytes_transferred = 0;
    uint32_t limit = (object_meta.file_size < max_bytes) ? object_meta.file_size : max_bytes;

    while (current_cluster >= 2 && current_cluster < 0x0FFFFFF8 && bytes_transferred < limit) {
        uint32_t sector = cluster_to_sector(fs, current_cluster);
        if (!volume_read_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_buf)) { kfree(cluster_buf); return 0; }

        uint32_t remaining = limit - bytes_transferred;
        uint32_t chunk_size = (remaining < cluster_bytes) ? remaining : cluster_bytes;
        memcpy(out_buffer + bytes_transferred, cluster_buf, chunk_size);
        bytes_transferred += chunk_size;

        current_cluster = fat32_get_next_cluster(fs, current_cluster);
    }
    kfree(cluster_buf);
    return 1;
}

int fat32_create_file_lfn(fat32_fs_t* fs, uint32_t parent_dir_cluster, const char* lfn_name, uint8_t attr) {
    int lfn_len = strlen(lfn_name);
    if (lfn_len == 0 || lfn_len > 255) return 0;

    int lfn_slots_needed = (lfn_len + 12) / 13;
    int total_slots_needed = lfn_slots_needed + 1;

    char sfn[11];
    fat32_generate_sfn(lfn_name, sfn);
    uint8_t sfn_chk = fat32_checksum_sfn(sfn);

    uint32_t first_cluster = fat32_allocate_cluster(fs);
    if (first_cluster == 0) return 0;

    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    void* dir_buf = kmalloc(cluster_bytes);
    if (!dir_buf) return 0;
    uint64_t phys_buf = (uint64_t)dir_buf - (uint64_t)HHDM_OFFSET;

    uint32_t sector = cluster_to_sector(fs, parent_dir_cluster);
    if (!volume_read_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_buf)) { kfree(dir_buf); return 0; }

    fat32_entry_t* entries = (fat32_entry_t*)dir_buf;
    uint32_t max_slots = cluster_bytes / 32;
    int target_index = -1;

    for (uint32_t i = 0; i <= max_slots - total_slots_needed; i++) {
        int continuous_found = 1;
        for (int sub = 0; sub < total_slots_needed; sub++) {
            uint8_t first_byte = (uint8_t)entries[i + sub].name[0];
            if (first_byte != FAT32_END_OF_DIR && first_byte != FAT32_FREE_ENTRY) { continuous_found = 0; break; }
        }
        if (continuous_found) { target_index = i; break; }
    }

    if (target_index == -1) { kfree(dir_buf); return 0; }

    int char_offset = (lfn_slots_needed - 1) * 13;
    for (int slot = 0; slot < lfn_slots_needed; slot++) {
        fat32_lfn_t* lfn_entry = (fat32_lfn_t*)&entries[target_index + slot];
        memset(lfn_entry, 0xFF, sizeof(fat32_lfn_t));
        
        uint8_t seq = (lfn_slots_needed - slot);
        if (slot == 0) seq |= 0x40;
        lfn_entry->sequence_number = seq;
        lfn_entry->attr = FAT_ATTR_LONG_NAME;
        lfn_entry->type = 0;
        lfn_entry->checksum = sfn_chk;
        lfn_entry->first_cluster = 0;

        int lfn_idx = char_offset;
        for (int c = 0; c < 5; c++) {
            if (lfn_idx < lfn_len) lfn_entry->name_characters1[c] = (uint16_t)lfn_name[lfn_idx++];
            else if (lfn_idx == lfn_len) { lfn_entry->name_characters1[c] = 0x0000; lfn_idx++; }
        }
        for (int c = 0; c < 6; c++) {
            if (lfn_idx < lfn_len) lfn_entry->name_characters2[c] = (uint16_t)lfn_name[lfn_idx++];
            else if (lfn_idx == lfn_len) { lfn_entry->name_characters2[c] = 0x0000; lfn_idx++; }
        }
        for (int c = 0; c < 2; c++) {
            if (lfn_idx < lfn_len) lfn_entry->name_characters3[c] = (uint16_t)lfn_name[lfn_idx++];
            else if (lfn_idx == lfn_len) { lfn_entry->name_characters3[c] = 0x0000; lfn_idx++; }
        }
        char_offset -= 13;
    }

    fat32_entry_t* final_sfn = &entries[target_index + lfn_slots_needed];
    memset(final_sfn, 0, sizeof(fat32_entry_t));
    memcpy(final_sfn->name, sfn, 11);
    final_sfn->attr = attr;
    final_sfn->first_cluster_high = (uint16_t)(first_cluster >> 16);
    final_sfn->first_cluster_low = (uint16_t)(first_cluster & 0xFFFF);
    final_sfn->file_size = 0;

    if (!volume_write_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_buf)) { kfree(dir_buf); return 0; }
    kfree(dir_buf);
    return 1;
}

int fat32_write_file_lfn(fat32_fs_t* fs, uint32_t dir_cluster, const char* filename, const uint8_t* in_buffer, uint32_t total_bytes) {
    fat32_entry_t object_meta;
    uint32_t start_cluster = fat32_find_object_lfn(fs, dir_cluster, filename, &object_meta);
    
    // Auto-create file via LFN pipeline if the target item does not exist
    if (start_cluster == 0) {
        if (!fat32_create_file_lfn(fs, dir_cluster, filename, FAT_ATTR_ARCHIVE)) return 0;
        start_cluster = fat32_find_object_lfn(fs, dir_cluster, filename, &object_meta);
        if (start_cluster == 0) return 0;
    }

    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    void* cluster_buf = kmalloc(cluster_bytes);
    if (!cluster_buf) return 0;
    uint64_t phys_buf = (uint64_t)cluster_buf - (uint64_t)HHDM_OFFSET;

    uint32_t current_cluster = start_cluster;
    uint32_t bytes_written = 0;

    while (bytes_written < total_bytes) {
        uint32_t chunk_size = (total_bytes - bytes_written < cluster_bytes) ? (total_bytes - bytes_written) : cluster_bytes;
        memset(cluster_buf, 0, cluster_bytes);
        memcpy(cluster_buf, in_buffer + bytes_written, chunk_size);

        uint32_t sector = cluster_to_sector(fs, current_cluster);
        if (!volume_write_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_buf)) { kfree(cluster_buf); return 0; }

        bytes_written += chunk_size;
        if (bytes_written >= total_bytes) break;

        uint32_t next = fat32_get_next_cluster(fs, current_cluster);
        if (next >= 0x0FFFFFF8) {
            next = fat32_allocate_cluster(fs);
            if (next == 0) { kfree(cluster_buf); return 0; }
            fat32_set_cluster_value(fs, current_cluster, next);
        }
        current_cluster = next;
    }
    kfree(cluster_buf);

    // Update file size metadata in the parent directory tracking structures
    void* dir_buf = kmalloc(cluster_bytes);
    if (!dir_buf) return 1;
    uint64_t phys_dir = (uint64_t)dir_buf - (uint64_t)HHDM_OFFSET;
    uint32_t dir_sec = cluster_to_sector(fs, dir_cluster);
    
    if (volume_read_sectors(fs->vol, dir_sec, fs->sectors_per_cluster, phys_dir)) {
        fat32_entry_t* entries = (fat32_entry_t*)dir_buf;
        uint32_t max_slots = cluster_bytes / 32;
        char target_sfn[11]; fat32_generate_sfn(filename, target_sfn);
        
        for (uint32_t i = 0; i < max_slots; i++) {
            if (entries[i].name[0] == FAT32_END_OF_DIR) break;
            if (memcmp(entries[i].name, target_sfn, 11) == 0 && entries[i].attr != FAT_ATTR_LONG_NAME) {
                entries[i].file_size = total_bytes;
                volume_write_sectors(fs->vol, dir_sec, fs->sectors_per_cluster, phys_dir);
                break;
            }
        }
    }
    kfree(dir_buf);
    return 1;
}

int fat32_create_directory_lfn(fat32_fs_t* fs, uint32_t parent_dir_cluster, const char* lfn_name) {
    if (!fat32_create_file_lfn(fs, parent_dir_cluster, lfn_name, FAT_ATTR_DIRECTORY)) return 0;
    
    fat32_entry_t meta;
    uint32_t new_dir_cluster = fat32_find_object_lfn(fs, parent_dir_cluster, lfn_name, &meta);
    if (new_dir_cluster == 0) return 0;

    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    void* clear_buf = kmalloc(cluster_bytes);
    if (!clear_buf) return 0;
    memset(clear_buf, 0, cluster_bytes);
    uint64_t phys_clear = (uint64_t)clear_buf - (uint64_t)HHDM_OFFSET;

    uint32_t sector = cluster_to_sector(fs, new_dir_cluster);
    if (!volume_write_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_clear)) { kfree(clear_buf); return 0; }
    kfree(clear_buf);

    // Inject matching structural dot self and dot-dot parent links using simple raw structures
    void* sub_buf = kmalloc(cluster_bytes);
    if (!sub_buf) return 0;
    memset(sub_buf, 0, cluster_bytes);
    fat32_entry_t* sub_ents = (fat32_entry_t*)sub_buf;

    memcpy(sub_ents[0].name, ".          ", 11);
    sub_ents[0].attr = FAT_ATTR_DIRECTORY;
    sub_ents[0].first_cluster_high = (uint16_t)(new_dir_cluster >> 16);
    sub_ents[0].first_cluster_low = (uint16_t)(new_dir_cluster & 0xFFFF);

    memcpy(sub_ents[1].name, "..         ", 11);
    sub_ents[1].attr = FAT_ATTR_DIRECTORY;
    sub_ents[1].first_cluster_high = (uint16_t)(parent_dir_cluster >> 16);
    sub_ents[1].first_cluster_low = (uint16_t)(parent_dir_cluster & 0xFFFF);

    uint64_t phys_sub = (uint64_t)sub_buf - (uint64_t)HHDM_OFFSET;
    volume_write_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_sub);
    kfree(sub_buf);
    return 1;
}

int fat32_remove_object_lfn(fat32_fs_t* fs, uint32_t parent_dir_cluster, const char* target_name) {
    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    void* dir_buf = kmalloc(cluster_bytes);
    if (!dir_buf) return 0;
    uint64_t phys_buf = (uint64_t)dir_buf - (uint64_t)HHDM_OFFSET;

    uint32_t sector = cluster_to_sector(fs, parent_dir_cluster);
    if (!volume_read_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_buf)) { kfree(dir_buf); return 0; }

    fat32_entry_t* entries = (fat32_entry_t*)dir_buf;
    uint32_t max_slots = cluster_bytes / 32;
    
    char lfn_accumulator[256]; memset(lfn_accumulator, 0, 256);
    int lfn_start_idx = -1;

    for (uint32_t i = 0; i < max_slots; i++) {
        if (entries[i].name[0] == FAT32_END_OF_DIR) break;
        if (entries[i].name[0] == (char)FAT32_FREE_ENTRY) { memset(lfn_accumulator, 0, 256); lfn_start_idx = -1; continue; }

        if (entries[i].attr == FAT_ATTR_LONG_NAME) {
            if (lfn_start_idx == -1) lfn_start_idx = i;
            fat32_lfn_t* lfn = (fat32_lfn_t*)&entries[i];
            fat32_extract_lfn_chars(lfn, lfn_accumulator);
        } else {
            char standard_name[13]; memset(standard_name, 0, 13); int p = 0;
            for (int c = 0; c < 8; c++) if (entries[i].name[c] != ' ') standard_name[p++] = entries[i].name[c];
            if (entries[i].name[8] != ' ') {
                standard_name[p++] = '.';
                for (int c = 8; c < 11; c++) if (entries[i].name[c] != ' ') standard_name[p++] = entries[i].name[c];
            }

            if (strcmp(lfn_accumulator, target_name) == 0 || strcmp(standard_name, target_name) == 0) {
                uint32_t target_cluster = ((uint32_t)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
                
                // Free the target object's payload data clusters
                uint32_t curr = target_cluster;
                while (curr >= 2 && curr < 0x0FFFFFF8) {
                    uint32_t next = fat32_get_next_cluster(fs, curr);
                    fat32_set_cluster_value(fs, curr, 0);
                    curr = next;
                }

                // Scrub the SFN entry record
                entries[i].name[0] = (char)FAT32_FREE_ENTRY;

                // Scrub preceding linked LFN entries if they exist
                if (lfn_start_idx != -1) {
                    for (int k = lfn_start_idx; k < (int)i; k++) {
                        entries[k].name[0] = (char)FAT32_FREE_ENTRY;
                    }
                }

                volume_write_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_buf);
                kfree(dir_buf);
                return 1;
            }
            memset(lfn_accumulator, 0, 256); lfn_start_idx = -1;
        }
    }
    kfree(dir_buf);
    return 0;
}

int fat32_rename_or_move_lfn(fat32_fs_t* fs, uint32_t src_dir, const char* old_name, uint32_t dest_dir, const char* new_name) {
    fat32_entry_t old_meta;
    uint32_t object_cluster = fat32_find_object_lfn(fs, src_dir, old_name, &old_meta);
    if (object_cluster == 0) return 0;

    // Create the updated entry profile line layout inside the destination directory block
    if (!fat32_create_file_lfn(fs, dest_dir, new_name, old_meta.attr)) return 0;

    // Direct metadata copy adjustments (Cluster routing & size profiles)
    uint32_t dest_sec = cluster_to_sector(fs, dest_dir);
    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    void* dest_buf = kmalloc(cluster_bytes);
    if (dest_buf) {
        uint64_t phys_dest = (uint64_t)dest_buf - (uint64_t)HHDM_OFFSET;
        if (volume_read_sectors(fs->vol, dest_sec, fs->sectors_per_cluster, phys_dest)) {
            fat32_entry_t* entries = (fat32_entry_t*)dest_buf;
            uint32_t max_slots = cluster_bytes / 32;
            char target_sfn[11]; fat32_generate_sfn(new_name, target_sfn);
            
            for (uint32_t i = 0; i < max_slots; i++) {
                if (entries[i].name[0] == FAT32_END_OF_DIR) break;
                if (memcmp(entries[i].name, target_sfn, 11) == 0 && entries[i].attr != FAT_ATTR_LONG_NAME) {
                    // Overwrite the newly allocated blank cluster links with original content anchors
                    entries[i].first_cluster_high = old_meta.first_cluster_high;
                    entries[i].first_cluster_low = old_meta.first_cluster_low;
                    entries[i].file_size = old_meta.file_size;
                    volume_write_sectors(fs->vol, dest_sec, fs->sectors_per_cluster, phys_dest);
                    break;
                }
            }
        }
        kfree(dest_buf);
    }

    // Safely delete the original entry pointers from the source directory
    return fat32_remove_object_lfn(fs, src_dir, old_name);
}