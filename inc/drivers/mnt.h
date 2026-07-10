#pragma once
#include <drivers/vfs.h>
#include <drivers/fat32.h>
typedef enum {
    VFS,
    FAT32
} PartitionType;
typedef struct {
    PartitionType type;
    union {
        fat32_fs_t fat;
    };
} partition_t;

typedef struct {
    partition_t part;
    char path[256];
    int8_t allocated;
} mountpoint_t;
typedef struct {
    struct vfs_file ord;
    mountpoint_t mountpoint;
    char resolved[256];
    int file_cluster;
    int dir_cluster;
} full_t;
int8_t mount(partition_t *partition, char *path);
void umount(int8_t mnt);
int open(char* path);
int read(int fd, void *buf, size_t count, uint64_t offset);
int write(int fd, const void *data, uint64_t size);