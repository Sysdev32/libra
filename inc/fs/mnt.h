#pragma once
#include <fs/vfs.h>
#include <fs/fat32.h>
#define DEVFS_IOCTL BIT(0)
#define DEVFS_READ BIT(1)
#define DEVFS_WRITE BIT(2)
#define AHCI_FLAGS   (DEVFS_READ | DEVFS_WRITE | DEVFS_IOCTL)
#define FB_FLAGS     (DEVFS_WRITE | DEVFS_IOCTL)
#define DISK_FLAGS   (DEVFS_READ | DEVFS_WRITE | DEVFS_IOCTL)
#define KBD_FLAGS    (DEVFS_READ | DEVFS_IOCTL)
#define MOUSE_FLAGS  (DEVFS_READ | DEVFS_IOCTL)
#define ZERO_FLAGS   (DEVFS_READ)
#define NULLD_FLAGS  (DEVFS_WRITE)
#define ETH_FLAGS    (DEVFS_READ | DEVFS_WRITE | DEVFS_IOCTL)
typedef enum {
    VFS,
    FAT32,
    CHFS,
    DEVFS
} PartitionType;
typedef struct {
    PartitionType type;
    union {
        fat32_fs_t fat;
        int chfs;
    };
} partition_t;

typedef struct {
    partition_t part;
    char path[256];
    int8_t allocated;
} mountpoint_t;
typedef enum {
    AHCI,
    FB,
    DISK,
    KBD,
    MOUSE,
    ZERO,
    NULLD,
    ETH
} DevFsType;
typedef size_t (*read_func_t)(int fd, void *buf, size_t count, int offset);
typedef size_t (*write_func_t)(int fd, const void *buf, size_t count);
typedef int (*ioctl_func_t)(int fd, unsigned long request, void *arg);
typedef struct {
    DevFsType type;
    uint8_t bitmask;
    read_func_t read;
    write_func_t write;
    ioctl_func_t ioctl;
    ahci_device_t dev;
    int tty;
    char name[16];
    bool allocated;
} devfs_file;
typedef struct {
    struct vfs_file ord;
    mountpoint_t mountpoint;
    char resolved[256];
    int file_cluster;
    int dir_cluster;
    devfs_file file;
} full_t;
int8_t mount(partition_t *partition, char *path);
void umount(int8_t mnt);
int open(char* path);
int read(int fd, void *buf, size_t count, uint64_t offset);
int write(int fd, const void *data, uint64_t size);
int create(char* path);
void register_device(read_func_t read, ioctl_func_t ioctl, write_func_t write,
                     uint8_t bitmask, DevFsType type, char* name,
                     ahci_device_t dev, int tty);
extern full_t fd_table[32];
extern devfs_file files[64];
