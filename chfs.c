#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#define SECTOR_SIZE 512
#define BLOCK_SIZE  4096

#define ATTR_DELETED  (1 << 0)
#define ATTR_FILE     (1 << 1)
#define ATTR_FOLDER   (1 << 2)

#pragma pack(push, 1)

typedef struct {
    uint32_t magic; // 0xDEADBEEF
    uint32_t relative_header_table_addr; 
    uint32_t relative_inode_table_addr; 
    uint32_t relative_indexing_table; 
    uint32_t relative_journal; 
    uint32_t relative_journal_indexing; 
    uint32_t relative_free_fhdr;
    uint32_t relative_free_inodes;
    uint16_t file_count;
    uint32_t inode_count;
    uint64_t journal_count;
    uint8_t version; // 6
} CHFS_HDR;

typedef struct {
    int64_t tv_nsec;
    int64_t tv_sec;
} timespec_t;

typedef struct {
    char path[64];
    uint32_t inode_indexes[524288];
    uint32_t inode_count;
    uint64_t size;
    uint32_t parent_inode_index;
    uint16_t attr;
} CHFS_FHDR;

typedef struct {
    bool root;
    uint32_t mode;
    uint8_t uid;
    uint8_t gid;
    uint64_t size;
    timespec_t atime;
    timespec_t mtime;
    timespec_t ctime;
    uint32_t crc32;
    uint8_t payload[4096];
} CHFS_IN;

#pragma pack(pop)

// Temporary internal structure to hold info while scanning directory tree
typedef struct {
    char host_full_path[512];
    char chfs_internal_path[64];
    size_t file_size;
} file_entry_t;

file_entry_t file_list[1024];
int global_file_count = 0;

// Simple CRC32 function matching your driver implementation
static uint32_t compute_crc32(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFF;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

// Recursively scan host directory to compile flat file array
void scan_directory(const char *base_path, const char *current_rel_path) {
    char path[1024];
    struct dirent *dp;
    DIR *dir = opendir(base_path);

    if (!dir) return;

    while ((dp = readdir(dir)) != NULL) {
        if (strcmp(dp->d_name, ".") != 0 && strcmp(dp->d_name, "..") != 0) {
            struct stat statbuf;
            snprintf(path, sizeof(path), "%s/%s", base_path, dp->d_name);
            stat(path, &statbuf);

            char new_rel_path[64];
            if (strlen(current_rel_path) == 0) {
                snprintf(new_rel_path, sizeof(new_rel_path), "/%s", dp->d_name);
            } else {
                snprintf(new_rel_path, sizeof(new_rel_path), "%s/%s", current_rel_path, dp->d_name);
            }

            if (S_ISDIR(statbuf.st_mode)) {
                // If you want explicit directory FHDRs created, handle them here.
                // For now, recursively walk to grab underlying files.
                scan_directory(path, new_rel_path);
            } else if (S_ISREG(statbuf.st_mode)) {
                if (global_file_count >= 1024) {
                    fprintf(stderr, "Warning: Exceeded temporary tool limit of 1024 files.\n");
                    break;
                }
                strncpy(file_list[global_file_count].host_full_path, path, 511);
                strncpy(file_list[global_file_count].chfs_internal_path, new_rel_path, 63);
                file_list[global_file_count].file_size = statbuf.st_size;
                global_file_count++;
            }
        }
    }
    closedir(dir);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <input_folder_path> <output_image.bin>\n", argv[0]);
        return 1;
    }

    const char *input_dir = argv[1];
    const char *output_bin = argv[2];

    printf("Scanning host directory: %s\n", input_dir);
    scan_directory(input_dir, "");
    printf("Found %d files to package into filesystem.\n", global_file_count);

    FILE *out = fopen(output_bin, "wb");
    if (!out) {
        perror("Failed to open output image file");
        return 1;
    }

    // 1. Calculate structural block boundary sizes and offsets
    uint32_t header_table_offset = SECTOR_SIZE; // Directly after master block
    uint32_t header_table_size = global_file_count * sizeof(CHFS_FHDR);
    
    // Round table sizes up to sector size alignment boundaries
    uint32_t header_table_sectors = (header_table_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint32_t real_header_table_bytes = header_table_sectors * SECTOR_SIZE;

    uint32_t indexing_table_offset = header_table_offset + real_header_table_bytes;
    
    // Pass 1 to count how many total payload inodes we are writing out
    uint32_t total_inodes = 0;
    for (int i = 0; i < global_file_count; i++) {
        uint32_t needed = file_list[i].file_size / BLOCK_SIZE;
        if (file_list[i].file_size % BLOCK_SIZE != 0 || file_list[i].file_size == 0) {
            needed++;
        }
        total_inodes += needed;
    }

    uint32_t indexing_table_size = total_inodes * sizeof(uint32_t);
    uint32_t indexing_table_sectors = (indexing_table_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint32_t real_indexing_table_bytes = indexing_table_sectors * SECTOR_SIZE;

    uint32_t journal_indexing_offset = indexing_table_offset + real_indexing_table_bytes;
    uint32_t real_journal_indexing_bytes = SECTOR_SIZE; // Dummy placeholder sector size for journal tracking

    uint32_t journal_offset = journal_indexing_offset + real_journal_indexing_bytes;
    uint32_t real_journal_bytes = SECTOR_SIZE; // Dummy placeholder sector size for journal contents

    uint32_t inode_table_offset = journal_offset + real_journal_bytes;

    // 2. Build and write the master file system volume header (LBA 0)
    CHFS_HDR master_hdr;
    memset(&master_hdr, 0, sizeof(CHFS_HDR));
    master_hdr.magic = 0xDEADBEEF;
    master_hdr.relative_header_table_addr = header_table_offset;
    master_hdr.relative_inode_table_addr = inode_table_offset;
    master_hdr.relative_indexing_table = indexing_table_offset;
    master_hdr.relative_journal = journal_offset;
    master_hdr.relative_journal_indexing = journal_indexing_offset;
    master_hdr.file_count = global_file_count;
    master_hdr.inode_count = total_inodes;
    master_hdr.journal_count = 0;
    master_hdr.version = 6;
    
    // Set dynamic write allocations tracking bounds to end of static blocks
    master_hdr.relative_free_fhdr = header_table_size; 
    master_hdr.relative_free_inodes = total_inodes * sizeof(CHFS_IN);

    fwrite(&master_hdr, sizeof(CHFS_HDR), 1, out);
    
    // Pad remaining master sector space to match 512 alignment boundary
    uint8_t zero_padding[SECTOR_SIZE];
    memset(zero_padding, 0, sizeof(zero_padding));
    fwrite(zero_padding, SECTOR_SIZE - sizeof(CHFS_HDR), 1, out);

    // 3. Construct elements and write out file records tables
    CHFS_FHDR *fhdr_array = calloc(header_table_sectors, SECTOR_SIZE);
    uint32_t *index_array = calloc(indexing_table_sectors, SECTOR_SIZE);

    uint32_t current_inode_global_idx = 0;
    uint32_t current_raw_inode_byte_offset = 0;

    for (int i = 0; i < global_file_count; i++) {
        strncpy(fhdr_array[i].path, file_list[i].chfs_internal_path, 63);
        fhdr_array[i].size = file_list[i].file_size;
        fhdr_array[i].attr = ATTR_FILE;
        fhdr_array[i].parent_inode_index = 0;

        uint32_t needed = file_list[i].file_size / BLOCK_SIZE;
        if (file_list[i].file_size % BLOCK_SIZE != 0 || file_list[i].file_size == 0) {
            needed++;
        }
        fhdr_array[i].inode_count = needed;

        for (uint32_t j = 0; j < needed; j++) {
            fhdr_array[i].inode_indexes[j] = current_inode_global_idx;
            index_array[current_inode_global_idx] = current_raw_inode_byte_offset;

            current_inode_global_idx++;
            current_raw_inode_byte_offset += sizeof(CHFS_IN);
        }
    }

    // Write file headers block
    fwrite(fhdr_array, real_header_table_bytes, 1, out);
    // Write inode structural offset allocation map indexing blocks
    fwrite(index_array, real_indexing_table_bytes, 1, out);

    // Write placeholder journal metadata tracking regions
    fwrite(zero_padding, real_journal_indexing_bytes, 1, out);
    fwrite(zero_padding, real_journal_bytes, 1, out);

    // 4. Serialize and append raw file payloads into CHFS_IN containers
    for (int i = 0; i < global_file_count; i++) {
        FILE *in_file = fopen(file_list[i].host_full_path, "rb");
        if (!in_file) {
            fprintf(stderr, "Failed to open input file: %s\n", file_list[i].host_full_path);
            continue;
        }

        size_t total_bytes_to_read = file_list[i].file_size;
        uint32_t inodes_for_file = fhdr_array[i].inode_count;

        for (uint32_t j = 0; j < inodes_for_file; j++) {
            CHFS_IN inode;
            memset(&inode, 0, sizeof(CHFS_IN));
            inode.root = false;
            inode.mode = 0644;

            size_t read_chunk = (total_bytes_to_read > BLOCK_SIZE) ? BLOCK_SIZE : total_bytes_to_read;
            if (read_chunk > 0) {
                size_t read_bytes = fread(inode.payload, 1, read_chunk, in_file);
                total_bytes_to_read -= read_bytes;
                inode.size = read_bytes;
            } else {
                inode.size = 0; // Empty file single block handle fallback
            }

            inode.crc32 = compute_crc32(inode.payload, inode.size);

            // Write continuous inode layout block out to image disk
            fwrite(&inode, sizeof(CHFS_IN), 1, out);
        }
        fclose(in_file);
    }

    // Wrap up image matching structural layout padding metrics
    long final_size = ftell(out);
    printf("Successfully packed CHFS image. Final binary footprint: %ld bytes\n", final_size);

    free(fhdr_array);
    free(index_array);
    fclose(out);

    return 0;
}