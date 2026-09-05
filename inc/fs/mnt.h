#ifndef FS_MNT_H
#define FS_MNT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <fs/vfs.h>
#include <fs/fat32.h>
#include <hals/ahci.h>

// DevFS Feature Bitmasks
#define DEVFS_IOCTL (1ULL << 0)
#define DEVFS_READ  (1ULL << 1)
#define DEVFS_WRITE (1ULL << 2)
#define O_RDONLY 0

// Device Flag Presets
#define AHCI_FLAGS   (DEVFS_READ | DEVFS_WRITE | DEVFS_IOCTL)
#define FB_FLAGS     (DEVFS_WRITE | DEVFS_IOCTL)
#define DISK_FLAGS   (DEVFS_READ | DEVFS_WRITE | DEVFS_IOCTL)
#define KBD_FLAGS    (DEVFS_READ | DEVFS_IOCTL)
#define MOUSE_FLAGS  (DEVFS_READ | DEVFS_IOCTL)
#define ZERO_FLAGS   (DEVFS_READ)
#define NULLD_FLAGS  (DEVFS_WRITE)
#define ETH_FLAGS    (DEVFS_READ | DEVFS_WRITE | DEVFS_IOCTL)

// Types & Structures
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
    initialized_drive *drive;
    int vol_index;
    char path[256];
    int8_t allocated;
    bool is_devfs;
} mountpoint_t;

typedef struct {
    read_func_t read;
    ioctl_func_t ioctl;
    write_func_t write;
    uint8_t bitmask;
    DevFsType type;
    ahci_device_t dev;
    int tty;
    char name[32];
    bool allocated;
} devfs_file;

typedef struct {
    struct vfs_file ord;
    mountpoint_t mountpoint;
    char resolved[256];
    uint32_t file_cluster;
    uint32_t dir_cluster;
    devfs_file file;
} full_t;

// Global Exports
extern mountpoint_t mountpoints[32];
extern full_t fd_table[32];
extern devfs_file files[64];
extern int last_fd;

// Core Mount API
int8_t get_lowest_mnt(void);
int8_t mount(initialized_drive *drive, int vol_index, char *path);
void umount(int8_t mnt);

// DevFS Operations
void devfs_init(void);
void register_device(read_func_t read, ioctl_func_t ioctl, write_func_t write,
                     uint8_t bitmask, DevFsType type, char* name,
                     ahci_device_t dev, int tty);

// Helper Utility Exports
size_t path_split(const char* src, char* dest_buf, char** out_tokens);
void string_shift(char* str, int shift_index);
const char* path_basename(const char* path);
size_t buffer_crop_to_output(const void* src, size_t src_total_sz, void* dest, size_t offset, size_t count);

// POSIX VFS Interface Functions
int open(char* path, int flags, uint32_t mode);
int read(int fd, void *buf, size_t count, uint64_t offset);
int write(int fd, const void *data, uint64_t size);
int create(char* path);
int close(int fd);

// Extended VFS Wrappers
int mnt_mkdir(const char *path, uint32_t mode);
int mnt_rmdir(const char *path);
int mnt_unlink(const char *path);
int mnt_stat(const char *path, struct vfs_stat *st);
int mnt_fstat(int fd, struct vfs_stat *st);
int mnt_getdents(int fd, void *buf, size_t count, uint64_t offset);
int mnt_listdir(const char *path, char **out_names, size_t max_entries);
#endif // FS_MNT_H