#pragma once
#include <stdint.h>
#include <drivers/gpt.h> // For volume_t

#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LONG_NAME (FAT_ATTR_READ_ONLY | FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM | FAT_ATTR_VOLUME_ID)

// FAT32 Extended Boot Record (EBR) / Bios Parameter Block (BPB)
typedef struct {
    uint8_t  bootjmp[3];
    char     oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    // FAT32 Extended Fields
    uint32_t sectors_per_fat_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     file_system_type[8];
} __attribute__((packed)) fat32_bpb_t;

// Standard FAT Directory Entry (Short File Name - SFN)
typedef struct {
    char     name[11]; 
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  creation_time_tenth;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
} __attribute__((packed)) fat32_entry_t;

// FAT Long File Name (LFN) Directory Entry
typedef struct {
    uint8_t  sequence_number; 
    uint16_t name_characters1[5]; 
    uint8_t  attr;            
    uint8_t  type;            
    uint8_t  checksum;        
    uint16_t name_characters2[6]; 
    uint16_t first_cluster;   
    uint16_t name_characters3[2]; 
} __attribute__((packed)) fat32_lfn_t;

// Driver Runtime State Struct
typedef struct {
    volume_t* vol;
    uint32_t  sectors_per_cluster;
    uint32_t  fat_start_sector;
    uint32_t  data_start_sector;
    uint32_t  root_cluster;
} fat32_fs_t;

/* --- Driver Core & Mechanics --- */
int      fat32_init(volume_t* vol, fat32_fs_t* fs);
uint32_t fat32_get_next_cluster(fat32_fs_t* fs, uint32_t current_cluster);
int      fat32_set_cluster_value(fat32_fs_t* fs, uint32_t cluster, uint32_t value);
uint32_t fat32_allocate_cluster(fat32_fs_t* fs);

/* --- Full LFN Virtual File System Interface --- */
void fat32_list_directory_lfn(fat32_fs_t* fs, uint32_t dir_cluster);
uint32_t fat32_find_object_lfn(fat32_fs_t* fs, uint32_t dir_cluster, const char* target_name, fat32_entry_t* out_entry);
int  fat32_read_file_lfn(fat32_fs_t* fs, uint32_t dir_cluster, const char* filename, uint8_t* out_buffer, uint32_t max_bytes);
int  fat32_write_file_lfn(fat32_fs_t* fs, uint32_t dir_cluster, const char* filename, const uint8_t* in_buffer, uint32_t total_bytes);
int  fat32_create_file_lfn(fat32_fs_t* fs, uint32_t parent_dir_cluster, const char* lfn_name, uint8_t attr);
int  fat32_create_directory_lfn(fat32_fs_t* fs, uint32_t parent_dir_cluster, const char* lfn_name);
int  fat32_remove_object_lfn(fat32_fs_t* fs, uint32_t parent_dir_cluster, const char* target_name);
int  fat32_rename_or_move_lfn(fat32_fs_t* fs, uint32_t src_dir, const char* old_name, uint32_t dest_dir, const char* new_name);