#include <fs/fat32.h>
#include <drivers/alloc.h>
#include <drivers/fb.h>
#include <string.h>
#include <stdbool.h>
#include <fs/vfs.h>

#define FAT32_EOF         0x0FFFFFF8
#define FAT32_FREE_ENTRY  0xE5
#define FAT32_END_OF_DIR  0x00

/* --- INTERNAL LFN HELPER FUNCTIONS --- */

static uint32_t cluster_to_sector(fat32_fs_t* fs, uint32_t cluster) {
    if (!fs || cluster < 2) return 0;
    return fs->data_start_sector + ((cluster - 2) * fs->sectors_per_cluster);
}

static uint8_t fat32_checksum_sfn(const char* sfn) {
    if (!sfn) return 0;
    uint8_t sum = 0;
    for (int i = 11; i > 0; i--) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)*sfn++;
    }
    return sum;
}

static bool fat32_is_valid_sfn_char(char c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    if (c == '$' || c == '%' || c == '\'' || c == '-' || c == '_' ||
        c == '@' || c == '~' || c == '`' || c == '!' || c == '(' || c == ')') {
        return true;
    }
    return false;
}

static void fat32_generate_sfn(const char* lfn, char* out_sfn) {
    if (!out_sfn) return;
    memset(out_sfn, ' ', 11);
    if (!lfn || *lfn == '\0') return;

    while (*lfn == '.' || *lfn == ' ') lfn++;

    const char* ext = strrchr(lfn, '.');
    size_t base_len = ext ? (size_t)(ext - lfn) : strlen(lfn);

    int sfn_pos = 0;
    size_t i = 0;

    while (i < base_len && sfn_pos < 6) {
        char c = lfn[i++];
        if (c >= 'a' && c <= 'z') c -= 32;

        if (fat32_is_valid_sfn_char(c)) {
            out_sfn[sfn_pos++] = c;
        } else if (c != ' ' && c != '.') {
            out_sfn[sfn_pos++] = '_';
        }
    }

    if (sfn_pos == 0) out_sfn[sfn_pos++] = '_';

    out_sfn[sfn_pos++] = '~';
    out_sfn[sfn_pos]   = '1';

    if (ext && strlen(ext) > 1) {
        ext++;
        int ext_pos = 8;
        while (*ext && ext_pos < 11) {
            char c = *ext++;
            if (c >= 'a' && c <= 'z') c -= 32;

            if (fat32_is_valid_sfn_char(c)) {
                out_sfn[ext_pos++] = c;
            } else if (c != ' ' && c != '.') {
                out_sfn[ext_pos++] = '_';
            }
        }
    }
}

static void extract_sfn_name(const char* raw_sfn, char* out_str) {
    if (!raw_sfn || !out_str) return;
    int p = 0;
    for (int c = 0; c < 8; c++) {
        if (raw_sfn[c] != ' ') out_str[p++] = raw_sfn[c];
    }
    if (raw_sfn[8] != ' ') {
        out_str[p++] = '.';
        for (int c = 8; c < 11; c++) {
            if (raw_sfn[c] != ' ') out_str[p++] = raw_sfn[c];
        }
    }
    out_str[p] = '\0';
}

static void extract_lfn_piece(fat32_lfn_t* lfn, char* accumulator) {
    if (!lfn || !accumulator) return;
    uint8_t raw_seq = (uint8_t)lfn->sequence_number;

    if (raw_seq == FAT32_FREE_ENTRY || raw_seq == FAT32_END_OF_DIR) return;

    uint8_t sequence = raw_seq & 0x1F;
    if (sequence < 1 || sequence > 20) return;

    uint32_t char_offset = (sequence - 1) * 13;

    uint16_t chars[13];
    memcpy(&chars[0],  lfn->name_characters1, 5 * sizeof(uint16_t));
    memcpy(&chars[5],  lfn->name_characters2, 6 * sizeof(uint16_t));
    memcpy(&chars[11], lfn->name_characters3, 2 * sizeof(uint16_t));

    for (int i = 0; i < 13; i++) {
        uint16_t c = chars[i];
        if (c == 0x0000 || c == 0xFFFF) {
            break;
        }

        uint32_t target_idx = char_offset + i;
        if (target_idx < 255) {
            accumulator[target_idx] = (char)(c & 0xFF);
        }
    }
}

/* --- CORE INITIALIZATION & LOW-LEVEL FAT MECHANICS --- */

int fat32_init(volume_t* vol, fat32_fs_t* fs) {
    if (!vol || !fs || !vol->is_valid) return 0;

    void* bpb_virt = kmalloc(512);
    if (!bpb_virt) return 0;
    uint64_t bpb_phys = (uint64_t)bpb_virt - (uint64_t)HHDM_OFFSET;

    if (!volume_read_sectors(vol, 0, 1, bpb_phys)) {
        kfree(bpb_virt);
        return 0;
    }

    fat32_bpb_t* bpb = (fat32_bpb_t*)bpb_virt;
    if (bpb->bytes_per_sector != 512 || bpb->sectors_per_cluster == 0) {
        kfree(bpb_virt);
        return 0;
    }

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
    if (!fs || current_cluster < 2 || current_cluster >= FAT32_EOF) return FAT32_EOF;

    uint32_t fat_offset = current_cluster * 4;
    uint32_t fat_sector = fs->fat_start_sector + (fat_offset / 512);
    uint32_t ent_offset = fat_offset % 512;

    void* sector_buf = kmalloc(512);
    if (!sector_buf) return FAT32_EOF;
    uint64_t phys_buf = (uint64_t)sector_buf - (uint64_t)HHDM_OFFSET;

    if (!volume_read_sectors(fs->vol, fat_sector, 1, phys_buf)) {
        kfree(sector_buf);
        return FAT32_EOF;
    }

    uint32_t next_cluster = *(uint32_t*)((uint8_t*)sector_buf + ent_offset) & 0x0FFFFFFF;
    kfree(sector_buf);
    return next_cluster;
}

int fat32_set_cluster_value(fat32_fs_t* fs, uint32_t cluster, uint32_t value) {
    if (!fs || cluster < 2 || cluster >= FAT32_EOF) return 0;

    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fs->fat_start_sector + (fat_offset / 512);
    uint32_t ent_offset = fat_offset % 512;

    void* sector_buf = kmalloc(512);
    if (!sector_buf) return 0;
    uint64_t phys_buf = (uint64_t)sector_buf - (uint64_t)HHDM_OFFSET;

    if (!volume_read_sectors(fs->vol, fat_sector, 1, phys_buf)) {
        kfree(sector_buf);
        return 0;
    }

    uint32_t* entry_ptr = (uint32_t*)((uint8_t*)sector_buf + ent_offset);
    *entry_ptr = (*entry_ptr & 0xF0000000) | (value & 0x0FFFFFFF);

    if (!volume_write_sectors(fs->vol, fat_sector, 1, phys_buf)) {
        kfree(sector_buf);
        return 0;
    }
    kfree(sector_buf);
    return 1;
}

uint32_t fat32_allocate_cluster(fat32_fs_t* fs) {
    if (!fs) return 0;

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

/* --- VFS VIRTUAL LAYER FOR LFN PROCESSING --- */

uint32_t fat32_find_object_lfn(fat32_fs_t* fs, uint32_t dir_cluster, const char* target_name, fat32_entry_t* out_entry) {
    if (!fs || !target_name || dir_cluster < 2 || dir_cluster >= FAT32_EOF) return 0;

    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    void* dir_buf = kmalloc(cluster_bytes);
    if (!dir_buf) return 0;
    uint64_t phys_buf = (uint64_t)dir_buf - (uint64_t)HHDM_OFFSET;

    char lfn_accumulator[256];
    memset(lfn_accumulator, 0, 256);

    uint32_t curr_cluster = dir_cluster;
    while (curr_cluster >= 2 && curr_cluster < FAT32_EOF) {
        uint32_t sector = cluster_to_sector(fs, curr_cluster);
        if (!sector || !volume_read_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_buf)) break;

        fat32_entry_t* entries = (fat32_entry_t*)dir_buf;
        uint32_t max_slots = cluster_bytes / 32;

        for (uint32_t i = 0; i < max_slots; i++) {
            if (entries[i].name[0] == FAT32_END_OF_DIR) {
                kfree(dir_buf);
                return 0;
            }
            if ((uint8_t)entries[i].name[0] == FAT32_FREE_ENTRY) {
                memset(lfn_accumulator, 0, 256);
                continue;
            }

            if (entries[i].attr == FAT_ATTR_LONG_NAME) {
                fat32_lfn_t* lfn = (fat32_lfn_t*)&entries[i];
                if (lfn->sequence_number & 0x40) {
                    memset(lfn_accumulator, 0, 256);
                }
                extract_lfn_piece(lfn, lfn_accumulator);
            } else {
                char final_name[256];
                memset(final_name, 0, 256);

                if (strlen(lfn_accumulator) > 0) {
                    strncpy(final_name, lfn_accumulator, 255);
                } else {
                    extract_sfn_name(entries[i].name, final_name);
                }

                if (strcmp(final_name, target_name) == 0) {
                    if (out_entry) *out_entry = entries[i];
                    uint32_t res_cluster = ((uint32_t)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
                    kfree(dir_buf);
                    return res_cluster;
                }
                memset(lfn_accumulator, 0, 256);
            }
        }
        curr_cluster = fat32_get_next_cluster(fs, curr_cluster);
    }
    kfree(dir_buf);
    return 0;
}

fat32_dir_entry_t** fat32_list_directory_lfn(fat32_fs_t* fs, uint32_t dir_cluster) {
    if (!fs || dir_cluster < 2 || dir_cluster >= FAT32_EOF) return NULL;

    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    void* dir_buf = kmalloc(cluster_bytes);
    if (!dir_buf) return NULL;
    uint64_t phys_buf = (uint64_t)dir_buf - (uint64_t)HHDM_OFFSET;

    uint32_t entry_count = 0;
    uint32_t entry_capacity = 16;
    fat32_dir_entry_t** result_list = kmalloc(entry_capacity * sizeof(fat32_dir_entry_t*));
    if (!result_list) {
        kfree(dir_buf);
        return NULL;
    }

    char lfn_accumulator[256];
    memset(lfn_accumulator, 0, 256);

    uint32_t curr_cluster = dir_cluster;
    bool stop_parsing = false;

    while (curr_cluster >= 2 && curr_cluster < FAT32_EOF && !stop_parsing) {
        uint32_t sector = cluster_to_sector(fs, curr_cluster);
        if (!sector || !volume_read_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_buf)) break;

        fat32_entry_t* entries = (fat32_entry_t*)dir_buf;
        uint32_t max_slots = cluster_bytes / 32;

        for (uint32_t i = 0; i < max_slots; i++) {
            if (entries[i].name[0] == FAT32_END_OF_DIR) {
                stop_parsing = true;
                break;
            }
            if ((uint8_t)entries[i].name[0] == FAT32_FREE_ENTRY) {
                memset(lfn_accumulator, 0, 256);
                continue;
            }

            if (entries[i].attr == FAT_ATTR_LONG_NAME) {
                fat32_lfn_t* lfn = (fat32_lfn_t*)&entries[i];
                if (lfn->sequence_number & 0x40) {
                    memset(lfn_accumulator, 0, 256);
                }
                extract_lfn_piece(lfn, lfn_accumulator);
            } else {
                if (entries[i].attr & FAT_ATTR_VOLUME_ID) {
                    memset(lfn_accumulator, 0, 256);
                    continue;
                }

                fat32_dir_entry_t* new_entry = kmalloc(sizeof(fat32_dir_entry_t));
                if (!new_entry) goto cleanup_error;

                new_entry->size = entries[i].file_size;
                new_entry->is_directory = (entries[i].attr & FAT_ATTR_DIRECTORY) ? true : false;
                memset(new_entry->name, 0, sizeof(new_entry->name));

                if (strlen(lfn_accumulator) > 0) {
                    strncpy(new_entry->name, lfn_accumulator, sizeof(new_entry->name) - 1);
                } else {
                    extract_sfn_name(entries[i].name, new_entry->name);
                }

                if (entry_count >= entry_capacity - 1) {
                    uint32_t new_capacity = entry_capacity * 2;
                    fat32_dir_entry_t** new_list = kmalloc(new_capacity * sizeof(fat32_dir_entry_t*));
                    if (!new_list) {
                        kfree(new_entry);
                        goto cleanup_error;
                    }

                    memcpy(new_list, result_list, entry_count * sizeof(fat32_dir_entry_t*));
                    kfree(result_list);
                    result_list = new_list;
                    entry_capacity = new_capacity;
                }

                result_list[entry_count++] = new_entry;
                memset(lfn_accumulator, 0, 256);
            }
        }
        if (!stop_parsing) {
            curr_cluster = fat32_get_next_cluster(fs, curr_cluster);
        }
    }

    kfree(dir_buf);
    result_list[entry_count] = NULL;
    return result_list;

cleanup_error:
    for (uint32_t i = 0; i < entry_count; i++) kfree(result_list[i]);
    kfree(result_list);
    kfree(dir_buf);
    return NULL;
}

int fat32_read_file_lfn(fat32_fs_t* fs, uint32_t dir_cluster, const char* filename, uint8_t* out_buffer, uint32_t max_bytes) {
    if (!fs || !filename || !out_buffer || dir_cluster < 2 || dir_cluster >= FAT32_EOF) return 0;

    fat32_entry_t object_meta;
    uint32_t start_cluster = fat32_find_object_lfn(fs, dir_cluster, filename, &object_meta);
    if (start_cluster < 2 || start_cluster >= FAT32_EOF || (object_meta.attr & FAT_ATTR_DIRECTORY)) return 0;

    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    void* cluster_buf = kmalloc(cluster_bytes);
    if (!cluster_buf) return 0;
    uint64_t phys_buf = (uint64_t)cluster_buf - (uint64_t)HHDM_OFFSET;

    uint32_t current_cluster = start_cluster;
    uint32_t bytes_transferred = 0;
    uint32_t limit = (object_meta.file_size < max_bytes) ? object_meta.file_size : max_bytes;

    while (current_cluster >= 2 && current_cluster < FAT32_EOF && bytes_transferred < limit) {
        uint32_t sector = cluster_to_sector(fs, current_cluster);
        if (!sector || !volume_read_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_buf)) {
            kfree(cluster_buf);
            return 0;
        }

        uint32_t remaining = limit - bytes_transferred;
        uint32_t chunk_size = (remaining < cluster_bytes) ? remaining : cluster_bytes;
        memcpy(out_buffer + bytes_transferred, cluster_buf, chunk_size);
        bytes_transferred += chunk_size;

        current_cluster = fat32_get_next_cluster(fs, current_cluster);
    }
    kfree(cluster_buf);
    return bytes_transferred;
}

int fat32_create_file_lfn(fat32_fs_t* fs, uint32_t parent_dir_cluster, const char* lfn_name, uint8_t attr) {
    if (!fs || !lfn_name || parent_dir_cluster < 2 || parent_dir_cluster >= FAT32_EOF) return 0;

    int lfn_len = strlen(lfn_name);
    if (lfn_len == 0 || lfn_len > 255) return 0;

    int lfn_slots_needed = (lfn_len + 12) / 13;
    int total_slots_needed = lfn_slots_needed + 1;

    char sfn[11];
    fat32_generate_sfn(lfn_name, sfn);
    uint8_t sfn_chk = fat32_checksum_sfn(sfn);

    uint32_t first_cluster = 0;
    if (!(attr & FAT_ATTR_DIRECTORY)) {
        first_cluster = fat32_allocate_cluster(fs);
        if (first_cluster < 2) return 0;
    }

    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    void* dir_buf = kmalloc(cluster_bytes);
    if (!dir_buf) return 0;
    uint64_t phys_buf = (uint64_t)dir_buf - (uint64_t)HHDM_OFFSET;

    uint32_t curr_cluster = parent_dir_cluster;
    uint32_t target_cluster = 0;
    int target_index = -1;

    while (curr_cluster >= 2 && curr_cluster < FAT32_EOF) {
        uint32_t sector = cluster_to_sector(fs, curr_cluster);
        if (!sector || !volume_read_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_buf)) {
            kfree(dir_buf);
            return 0;
        }

        fat32_entry_t* entries = (fat32_entry_t*)dir_buf;
        uint32_t max_slots = cluster_bytes / 32;
        int continuous_free = 0;
        int start_slot = -1;

        for (uint32_t i = 0; i < max_slots; i++) {
            uint8_t first_byte = (uint8_t)entries[i].name[0];
            if (first_byte == FAT32_END_OF_DIR || first_byte == FAT32_FREE_ENTRY) {
                if (continuous_free == 0) start_slot = i;
                continuous_free++;
                if (continuous_free == total_slots_needed) {
                    target_index = start_slot;
                    target_cluster = curr_cluster;
                    break;
                }
            } else {
                continuous_free = 0;
                start_slot = -1;
            }
        }

        if (target_index != -1) break;

        uint32_t next = fat32_get_next_cluster(fs, curr_cluster);
        if (next >= FAT32_EOF) {
            uint32_t new_dir_clus = fat32_allocate_cluster(fs);
            if (new_dir_clus < 2) {
                kfree(dir_buf);
                return 0;
            }

            fat32_set_cluster_value(fs, curr_cluster, new_dir_clus);

            memset(dir_buf, 0, cluster_bytes);
            uint32_t new_sec = cluster_to_sector(fs, new_dir_clus);
            if (new_sec) {
                volume_write_sectors(fs->vol, new_sec, fs->sectors_per_cluster, phys_buf);
            }

            target_cluster = new_dir_clus;
            target_index = 0;
            break;
        }
        curr_cluster = next;
    }

    if (target_index == -1 || target_cluster < 2) {
        kfree(dir_buf);
        return 0;
    }

    uint32_t target_sec = cluster_to_sector(fs, target_cluster);
    if (!target_sec || !volume_read_sectors(fs->vol, target_sec, fs->sectors_per_cluster, phys_buf)) {
        kfree(dir_buf);
        return 0;
    }

    fat32_entry_t* entries = (fat32_entry_t*)dir_buf;

    for (int slot = 0; slot < lfn_slots_needed; slot++) {
        fat32_lfn_t* lfn_entry = (fat32_lfn_t*)&entries[target_index + slot];
        memset(lfn_entry, 0xFF, sizeof(fat32_lfn_t));

        int seq_num = lfn_slots_needed - slot;
        uint8_t seq_byte = (uint8_t)seq_num;
        if (slot == 0) seq_byte |= 0x40;

        lfn_entry->sequence_number = seq_byte;
        lfn_entry->attr = FAT_ATTR_LONG_NAME;
        lfn_entry->type = 0;
        lfn_entry->checksum = sfn_chk;
        lfn_entry->first_cluster = 0;

        int char_start = (seq_num - 1) * 13;

        for (int c = 0; c < 5; c++) {
            int idx = char_start + c;
            if (idx < lfn_len) lfn_entry->name_characters1[c] = (uint16_t)(uint8_t)lfn_name[idx];
            else if (idx == lfn_len) lfn_entry->name_characters1[c] = 0x0000;
            else lfn_entry->name_characters1[c] = 0xFFFF;
        }

        for (int c = 0; c < 6; c++) {
            int idx = char_start + 5 + c;
            if (idx < lfn_len) lfn_entry->name_characters2[c] = (uint16_t)(uint8_t)lfn_name[idx];
            else if (idx == lfn_len) lfn_entry->name_characters2[c] = 0x0000;
            else lfn_entry->name_characters2[c] = 0xFFFF;
        }

        for (int c = 0; c < 2; c++) {
            int idx = char_start + 11 + c;
            if (idx < lfn_len) lfn_entry->name_characters3[c] = (uint16_t)(uint8_t)lfn_name[idx];
            else if (idx == lfn_len) lfn_entry->name_characters3[c] = 0x0000;
            else lfn_entry->name_characters3[c] = 0xFFFF;
        }
    }

    fat32_entry_t* final_sfn = &entries[target_index + lfn_slots_needed];
    memset(final_sfn, 0, sizeof(fat32_entry_t));
    memcpy(final_sfn->name, sfn, 11);
    final_sfn->attr = attr;
    final_sfn->first_cluster_high = (uint16_t)(first_cluster >> 16);
    final_sfn->first_cluster_low = (uint16_t)(first_cluster & 0xFFFF);
    final_sfn->file_size = 0;

    if (!volume_write_sectors(fs->vol, target_sec, fs->sectors_per_cluster, phys_buf)) {
        kfree(dir_buf);
        return 0;
    }

    kfree(dir_buf);
    return 1;
}

int fat32_create_directory_lfn(fat32_fs_t* fs, uint32_t parent_dir_cluster, const char* lfn_name) {
    if (!fs || !lfn_name || parent_dir_cluster < 2 || parent_dir_cluster >= FAT32_EOF) return 0;

    if (!fat32_create_file_lfn(fs, parent_dir_cluster, lfn_name, FAT_ATTR_DIRECTORY)) return 0;

    fat32_entry_t meta;
    uint32_t new_dir_cluster = fat32_find_object_lfn(fs, parent_dir_cluster, lfn_name, &meta);
    if (new_dir_cluster < 2 || new_dir_cluster >= FAT32_EOF) return 0;

    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    void* clear_buf = kmalloc(cluster_bytes);
    if (!clear_buf) return 0;
    memset(clear_buf, 0, cluster_bytes);
    uint64_t phys_clear = (uint64_t)clear_buf - (uint64_t)HHDM_OFFSET;

    uint32_t sector = cluster_to_sector(fs, new_dir_cluster);
    if (!sector || !volume_write_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_clear)) {
        kfree(clear_buf);
        return 0;
    }
    kfree(clear_buf);

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
    if (!fs || !target_name || parent_dir_cluster < 2 || parent_dir_cluster >= FAT32_EOF) return 0;

    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    void* dir_buf = kmalloc(cluster_bytes);
    if (!dir_buf) return 0;
    uint64_t phys_buf = (uint64_t)dir_buf - (uint64_t)HHDM_OFFSET;

    uint32_t sector = cluster_to_sector(fs, parent_dir_cluster);
    if (!sector || !volume_read_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_buf)) {
        kfree(dir_buf);
        return 0;
    }

    fat32_entry_t* entries = (fat32_entry_t*)dir_buf;
    uint32_t max_slots = cluster_bytes / 32;

    char lfn_accumulator[256];
    memset(lfn_accumulator, 0, 256);
    int lfn_start_idx = -1;

    for (uint32_t i = 0; i < max_slots; i++) {
        if (entries[i].name[0] == FAT32_END_OF_DIR) break;
        if ((uint8_t)entries[i].name[0] == FAT32_FREE_ENTRY) {
            memset(lfn_accumulator, 0, 256);
            lfn_start_idx = -1;
            continue;
        }

        if (entries[i].attr == FAT_ATTR_LONG_NAME) {
            fat32_lfn_t* lfn = (fat32_lfn_t*)&entries[i];
            if (lfn->sequence_number & 0x40) {
                memset(lfn_accumulator, 0, 256);
                lfn_start_idx = i;
            } else if (lfn_start_idx == -1) {
                lfn_start_idx = i;
            }
            extract_lfn_piece(lfn, lfn_accumulator);
        } else {
            char final_name[256];
            memset(final_name, 0, 256);

            if (strlen(lfn_accumulator) > 0) {
                strncpy(final_name, lfn_accumulator, 255);
            } else {
                extract_sfn_name(entries[i].name, final_name);
            }

            if (strcmp(final_name, target_name) == 0) {
                uint32_t target_cluster = ((uint32_t)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;

                uint32_t curr = target_cluster;
                while (curr >= 2 && curr < FAT32_EOF) {
                    uint32_t next = fat32_get_next_cluster(fs, curr);
                    fat32_set_cluster_value(fs, curr, 0);
                    curr = next;
                }

                entries[i].name[0] = (char)FAT32_FREE_ENTRY;

                if (lfn_start_idx != -1) {
                    for (int k = lfn_start_idx; k < (int)i; k++) {
                        entries[k].name[0] = (char)FAT32_FREE_ENTRY;
                    }
                }

                volume_write_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_buf);
                kfree(dir_buf);
                return 1;
            }
            memset(lfn_accumulator, 0, 256);
            lfn_start_idx = -1;
        }
    }
    kfree(dir_buf);
    return 0;
}

int fat32_rename_or_move_lfn(fat32_fs_t* fs, uint32_t src_dir, const char* old_name, uint32_t dest_dir, const char* new_name) {
    if (!fs || !old_name || !new_name || src_dir < 2 || src_dir >= FAT32_EOF || dest_dir < 2 || dest_dir >= FAT32_EOF) return 0;

    fat32_entry_t old_meta;
    uint32_t object_cluster = fat32_find_object_lfn(fs, src_dir, old_name, &old_meta);
    if (object_cluster == 0) return 0;

    if (!fat32_create_file_lfn(fs, dest_dir, new_name, old_meta.attr)) return 0;

    uint32_t dest_sec = cluster_to_sector(fs, dest_dir);
    if (!dest_sec) return 0;

    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    void* dest_buf = kmalloc(cluster_bytes);
    if (dest_buf) {
        uint64_t phys_dest = (uint64_t)dest_buf - (uint64_t)HHDM_OFFSET;
        if (volume_read_sectors(fs->vol, dest_sec, fs->sectors_per_cluster, phys_dest)) {
            fat32_entry_t* entries = (fat32_entry_t*)dest_buf;
            uint32_t max_slots = cluster_bytes / 32;
            char target_sfn[11];
            fat32_generate_sfn(new_name, target_sfn);

            for (uint32_t i = 0; i < max_slots; i++) {
                if (entries[i].name[0] == FAT32_END_OF_DIR) break;
                if (memcmp(entries[i].name, target_sfn, 11) == 0 && entries[i].attr != FAT_ATTR_LONG_NAME) {
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

    return fat32_remove_object_lfn(fs, src_dir, old_name);
}

int fat32_write_file_lfn(fat32_fs_t* fs, uint32_t dir_cluster, const char* filename, const uint8_t* in_buffer, uint32_t total_bytes) {
    if (!fs || !filename || (!in_buffer && total_bytes > 0) || dir_cluster < 2 || dir_cluster >= FAT32_EOF) return 0;

    fat32_entry_t entry;
    uint32_t first_cluster = fat32_find_object_lfn(fs, dir_cluster, filename, &entry);

    if (first_cluster == 0) {
        if (!fat32_create_file_lfn(fs, dir_cluster, filename, FAT_ATTR_ARCHIVE)) {
            return 0;
        }
        first_cluster = fat32_find_object_lfn(fs, dir_cluster, filename, &entry);
        if (first_cluster < 2 || first_cluster >= FAT32_EOF) return 0;
    } else if (entry.attr & FAT_ATTR_DIRECTORY) {
        return 0;
    }

    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    if (cluster_bytes == 0) return 0;

    uint32_t clusters_needed = (total_bytes + cluster_bytes - 1) / cluster_bytes;
    if (clusters_needed == 0) clusters_needed = 1;

    uint32_t current_cluster = first_cluster;
    uint32_t clusters_allocated = 0;

    while (clusters_allocated < clusters_needed) {
        clusters_allocated++;
        uint32_t next = fat32_get_next_cluster(fs, current_cluster);

        if (clusters_allocated < clusters_needed) {
            if (next >= FAT32_EOF || next < 2) {
                uint32_t new_cluster = fat32_allocate_cluster(fs);
                if (new_cluster < 2) return 0;

                fat32_set_cluster_value(fs, current_cluster, new_cluster);
                current_cluster = new_cluster;
            } else {
                current_cluster = next;
            }
        }
    }

    uint32_t next_to_free = fat32_get_next_cluster(fs, current_cluster);
    fat32_set_cluster_value(fs, current_cluster, FAT32_EOF);

    while (next_to_free >= 2 && next_to_free < FAT32_EOF) {
        uint32_t temp = fat32_get_next_cluster(fs, next_to_free);
        fat32_set_cluster_value(fs, next_to_free, 0);
        next_to_free = temp;
    }

    void* cluster_buf = kmalloc(cluster_bytes);
    if (!cluster_buf) return 0;
    uint64_t phys_buf = (uint64_t)cluster_buf - (uint64_t)HHDM_OFFSET;

    uint32_t bytes_written = 0;
    current_cluster = first_cluster;

    while (bytes_written < total_bytes && current_cluster >= 2 && current_cluster < FAT32_EOF) {
        uint32_t chunk_size = total_bytes - bytes_written;
        if (chunk_size > cluster_bytes) chunk_size = cluster_bytes;

        memset(cluster_buf, 0, cluster_bytes);
        memcpy(cluster_buf, in_buffer + bytes_written, chunk_size);

        uint32_t sector = cluster_to_sector(fs, current_cluster);
        if (!sector || !volume_write_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_buf)) {
            kfree(cluster_buf);
            return 0;
        }

        bytes_written += chunk_size;
        if (bytes_written < total_bytes) {
            current_cluster = fat32_get_next_cluster(fs, current_cluster);
        }
    }
    kfree(cluster_buf);

    void* dir_buf = kmalloc(cluster_bytes);
    if (!dir_buf) return 0;
    uint64_t phys_dir = (uint64_t)dir_buf - (uint64_t)HHDM_OFFSET;

    uint32_t curr_dir_cluster = dir_cluster;
    char target_sfn[11];
    fat32_generate_sfn(filename, target_sfn);

    bool metadata_updated = false;

    while (curr_dir_cluster >= 2 && curr_dir_cluster < FAT32_EOF && !metadata_updated) {
        uint32_t dir_sector = cluster_to_sector(fs, curr_dir_cluster);
        if (!dir_sector || !volume_read_sectors(fs->vol, dir_sector, fs->sectors_per_cluster, phys_dir)) break;

        fat32_entry_t* entries = (fat32_entry_t*)dir_buf;
        uint32_t max_slots = cluster_bytes / 32;

        for (uint32_t i = 0; i < max_slots; i++) {
            if (entries[i].name[0] == FAT32_END_OF_DIR) break;

            if (entries[i].attr != FAT_ATTR_LONG_NAME && memcmp(entries[i].name, target_sfn, 11) == 0) {
                entries[i].file_size = total_bytes;
                entries[i].first_cluster_high = (uint16_t)(first_cluster >> 16);
                entries[i].first_cluster_low = (uint16_t)(first_cluster & 0xFFFF);

                volume_write_sectors(fs->vol, dir_sector, fs->sectors_per_cluster, phys_dir);
                metadata_updated = true;
                break;
            }
        }
        curr_dir_cluster = fat32_get_next_cluster(fs, curr_dir_cluster);
    }

    kfree(dir_buf);
    return metadata_updated ? bytes_written : 0;
}

int fat32_read_dir_lfn(fat32_fs_t* fs, uint32_t dir_cluster, struct vfs_dirent* buf, size_t max_entries) {
    if (!fs || !buf || max_entries == 0 || dir_cluster < 2 || dir_cluster >= FAT32_EOF) return -1;

    uint32_t cluster_bytes = fs->sectors_per_cluster * 512;
    void* dir_buf = kmalloc(cluster_bytes);
    if (!dir_buf) return -1;
    uint64_t phys_buf = (uint64_t)dir_buf - (uint64_t)HHDM_OFFSET;

    char lfn_accumulator[256];
    memset(lfn_accumulator, 0, 256);

    uint32_t curr_cluster = dir_cluster;
    size_t entry_count = 0;
    bool stop_parsing = false;

    while (curr_cluster >= 2 && curr_cluster < FAT32_EOF && !stop_parsing && entry_count < max_entries) {
        uint32_t sector = cluster_to_sector(fs, curr_cluster);
        if (!sector || !volume_read_sectors(fs->vol, sector, fs->sectors_per_cluster, phys_buf)) break;

        fat32_entry_t* entries = (fat32_entry_t*)dir_buf;
        uint32_t max_slots = cluster_bytes / 32;

        for (uint32_t i = 0; i < max_slots && entry_count < max_entries; i++) {
            if (entries[i].name[0] == FAT32_END_OF_DIR) {
                stop_parsing = true;
                break;
            }
            if ((uint8_t)entries[i].name[0] == FAT32_FREE_ENTRY) {
                memset(lfn_accumulator, 0, 256);
                continue;
            }

            if (entries[i].attr == FAT_ATTR_LONG_NAME) {
                fat32_lfn_t* lfn = (fat32_lfn_t*)&entries[i];
                if (lfn->sequence_number & 0x40) {
                    memset(lfn_accumulator, 0, 256);
                }
                extract_lfn_piece(lfn, lfn_accumulator);
            } else {
                if (entries[i].attr & FAT_ATTR_VOLUME_ID) {
                    memset(lfn_accumulator, 0, 256);
                    continue;
                }

                struct vfs_dirent* vd = &buf[entry_count];
                vd->d_ino = ((uint32_t)entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
                vd->d_type = (entries[i].attr & FAT_ATTR_DIRECTORY) ? 2 : 1;

                memset(vd->d_name, 0, sizeof(vd->d_name));

                if (strlen(lfn_accumulator) > 0) {
                    strncpy(vd->d_name, lfn_accumulator, sizeof(vd->d_name) - 1);
                } else {
                    extract_sfn_name(entries[i].name, vd->d_name);
                }

                entry_count++;
                memset(lfn_accumulator, 0, 256);
            }
        }

        if (!stop_parsing) {
            curr_cluster = fat32_get_next_cluster(fs, curr_cluster);
        }
    }

    kfree(dir_buf);
    return (int)(entry_count * sizeof(struct vfs_dirent));
}

int fat32_format(volume_t* vol, const char* volume_label) {
    if (!vol || !vol->is_valid || vol->total_sectors < 65536) {
        return 0;
    }

    uint32_t total_sectors = vol->total_sectors;
    uint16_t reserved_sectors = 32;
    uint8_t num_fats = 2;
    uint8_t sectors_per_cluster = 8; // 4 KB clusters

    uint32_t bytes_per_fat_entry = 4;
    uint32_t entries_per_sector = 512 / bytes_per_fat_entry;
    uint32_t total_clusters_approx = total_sectors / sectors_per_cluster;
    uint32_t sectors_per_fat = (total_clusters_approx + entries_per_sector - 1) / entries_per_sector;

    void* sector_buf = kmalloc(512);
    if (!sector_buf) return 0;
    uint64_t phys_buf = (uint64_t)sector_buf - (uint64_t)HHDM_OFFSET;

    /* --- 1. Construct & Write Boot Sector (LBA 0) --- */
    memset(sector_buf, 0, 512);
    fat32_bpb_t* bpb = (fat32_bpb_t*)sector_buf;

    bpb->bootjmp[0] = 0xEB;
    bpb->bootjmp[1] = 0x58;
    bpb->bootjmp[2] = 0x90;
    memcpy(bpb->oem_name, "MSWIN4.1", 8);

    bpb->bytes_per_sector = 512;
    bpb->sectors_per_cluster = sectors_per_cluster;
    bpb->reserved_sector_count = reserved_sectors;
    bpb->num_fats = num_fats;
    bpb->media_type = 0xF8;
    bpb->total_sectors_16 = 0;
    bpb->total_sectors_32 = total_sectors;

    bpb->sectors_per_fat_32 = sectors_per_fat;
    bpb->root_cluster = 2;
    bpb->fs_info = 1;
    bpb->backup_boot_sector = 6;
    bpb->drive_number = 0x80;
    bpb->boot_signature = 0x29;
    bpb->volume_id = 0x12345678;

    if (volume_label && strlen(volume_label) > 0) {
        memset(bpb->volume_label, ' ', 11);
        size_t len = strlen(volume_label);
        if (len > 11) len = 11;
        memcpy(bpb->volume_label, volume_label, len);
    } else {
        memcpy(bpb->volume_label, "NO NAME    ", 11);
    }
    memcpy(bpb->file_system_type, "FAT32   ", 8);

    ((uint8_t*)sector_buf)[510] = 0x55;
    ((uint8_t*)sector_buf)[511] = 0xAA;

    if (!volume_write_sectors(vol, 0, 1, phys_buf) ||
        !volume_write_sectors(vol, 6, 1, phys_buf)) {
        kfree(sector_buf);
        return 0;
    }

    /* --- 2. Construct & Write FSInfo Sector (LBA 1 & Backup LBA 7) --- */
    memset(sector_buf, 0, 512);
    uint32_t* fsinfo = (uint32_t*)sector_buf;
    fsinfo[0] = 0x41615252;
    fsinfo[120] = 0x61417272;
    fsinfo[121] = (total_sectors - reserved_sectors - (num_fats * sectors_per_fat)) / sectors_per_cluster - 1;
    fsinfo[122] = 2;
    fsinfo[127] = 0xAA550000;

    if (!volume_write_sectors(vol, 1, 1, phys_buf) ||
        !volume_write_sectors(vol, 7, 1, phys_buf)) {
        kfree(sector_buf);
        return 0;
    }

    /* --- 3. Zero out Remaining Reserved Sectors --- */
    memset(sector_buf, 0, 512);
    for (uint16_t s = 2; s < reserved_sectors; s++) {
        if (s == 6 || s == 7) continue;
        if (!volume_write_sectors(vol, s, 1, phys_buf)) {
            kfree(sector_buf);
            return 0;
        }
    }

    /* --- 4. Initialize FAT Tables (FAT1 & FAT2) --- */
    uint32_t fat1_start = reserved_sectors;
    uint32_t fat2_start = fat1_start + sectors_per_fat;

    uint32_t* fat_sector = (uint32_t*)sector_buf;
    fat_sector[0] = 0x0FFFFF00 | 0xF8;
    fat_sector[1] = 0x0FFFFFFF;
    fat_sector[2] = 0x0FFFFFFF;

    if (!volume_write_sectors(vol, fat1_start, 1, phys_buf) ||
        !volume_write_sectors(vol, fat2_start, 1, phys_buf)) {
        kfree(sector_buf);
        return 0;
    }

    memset(sector_buf, 0, 512);
    for (uint32_t s = 1; s < sectors_per_fat; s++) {
        if (!volume_write_sectors(vol, fat1_start + s, 1, phys_buf) ||
            !volume_write_sectors(vol, fat2_start + s, 1, phys_buf)) {
            kfree(sector_buf);
            return 0;
        }
    }

    /* --- 5. Zero out Root Directory Cluster (Cluster 2) --- */
    uint32_t data_start_sector = fat1_start + (num_fats * sectors_per_fat);
    for (uint8_t s = 0; s < sectors_per_cluster; s++) {
        if (!volume_write_sectors(vol, data_start_sector + s, 1, phys_buf)) {
            kfree(sector_buf);
            return 0;
        }
    }

    kfree(sector_buf);
    return 1;
}