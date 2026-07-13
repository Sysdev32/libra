#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <fs/gpt.h>

#define ATTR_DELETED  (1 << 0)
#define ATTR_FILE     (1 << 1)
#define ATTR_FOLDER   (1 << 2)

#pragma pack(push, 1)

// CHFS: CPIO Hardware Filesystem
typedef struct {
    uint32_t magic;                      // Always 0xDEADBEEF
    uint32_t relative_header_table_addr; // Header table byte offset
    uint32_t relative_inode_table_addr;  // Inode table byte offset
    uint32_t relative_indexing_table;    // Array of uint32_t mapping active inodes
    uint32_t relative_journal;           // Journaling region byte offset
    uint32_t relative_journal_indexing;  // Array of uint32_t mapping journal entries
    uint32_t relative_free_fhdr;
    uint32_t relative_free_inodes;
    uint16_t file_count;
    uint32_t inode_count;
    uint64_t journal_count;
    uint8_t version;                     // Always 6
} CHFS_HDR;

typedef struct {
    int64_t tv_nsec;
    int64_t tv_sec;
} timespec;

typedef struct {
    char path[64];
    uint32_t inode_indexes[1024];        // Scaled down from 524288 to resolve 2MB allocation overflow
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
    timespec atime;
    timespec mtime;
    timespec ctime;
    uint32_t crc32;
    uint8_t payload[4096];
} CHFS_IN;

typedef enum {
    JRN_INALLOCATED = 0,
    JRN_WRITE,
    JRN_READ,
} CHFS_JRN_TYPE;

typedef struct {
    bool committed;
    CHFS_JRN_TYPE op;
    uint16_t length;
    uint8_t payload[4084];
} CHFS_JRN;

#pragma pack(pop)

int create_chfs(int disk, char* path);
int change_mode(int disk, char* path, uint32_t mode);
int write_chfs(int disk, char* path, void* buffer, size_t size);
int read_chfs(int disk, char* path, void* buffer, int offset, int count);
int add_partition(volume_t* vol);
int format_chfs(volume_t* vol);