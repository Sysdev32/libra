// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>
struct timespec {
    int32_t  tv_sec;   /* 32-bit seconds (Year 2038 compliant!) */
    int32_t  tv_nsec;  /* Nanoseconds */
};
#define MAX_DIR_CHILDREN 32  // Safe upper bound for files/folders inside a single directory

// The file tracking structure used in the global files array
struct vfs_file {
    char path[256];     // Fixed size local buffer! No kmalloc needed for names anymore!
    uint8_t *data;
    uint64_t size;
    uint32_t mode;
    uint32_t uid;       // ADDED: Owner User ID
    uint32_t gid;       // ADDED: Owner Group ID
    uint32_t nlink;     // ADDED: Number of hard links
    struct timespec st_atim;    /* Access time */
    struct timespec st_mtim;    /* Modification time */
    struct timespec st_ctim;    /* Status change time */
};

// Metadata storage for files and directories
struct inode {
    uint32_t ino_num;    // Unique identifier number
    uint32_t mode;       // Type (file/directory) and permissions
    uint32_t uid;        // ADDED: Owner User ID
    uint32_t gid;        // ADDED: Owner Group ID
    uint64_t size;       // Payload size in bytes
    uint64_t links;
    struct timespec st_atim;    /* Access time */
    struct timespec st_mtim;    /* Modification time */
    struct timespec st_ctim;    /* Status change time */
    const uint8_t *data; // Raw pointer directly to file payload bytes
};

// A tree-component directory block (The Dentry) - Completely krealloc free
struct dentry {
    char *name;              // Filename only (e.g. "main.py", NOT the full path)
    struct inode *inode;     // Pointer to metadata/payload
    struct dentry *parent;   // Pointer to parent directory dentry
    
    // Fixed array instead of dynamic pointer allocation to avoid heap bugs
    struct dentry *children[MAX_DIR_CHILDREN];
    size_t child_count;
};

// An execution descriptor instance (The File Object)
struct file {
    struct dentry *dentry;   // Points to path information
    uint64_t offset;         // Current read/write cursor tracking byte
    uint32_t flags;          // O_RDONLY, O_WRONLY, etc.
};

// Matches the standard concept of POSIX struct dirent
struct vfs_dirent {
    uint32_t d_ino;       // Inode number of the file
    uint32_t d_type;      // Type: 4 for Directory (DT_DIR), 8 for Regular File (DT_REG)
    char d_name[256];     // Null-terminated filename string
};

// Forward declarations for filesystem backends
struct vfs_mount;

// Filesystem operation table — each mounted filesystem provides these
struct fs_operations {
    int  (*open)(struct vfs_mount *mnt, const char *path);
    int  (*read)(struct vfs_mount *mnt, int local_fd, void *buf, size_t count, uint64_t offset);
    int  (*write)(struct vfs_mount *mnt, int local_fd, const void *data, uint64_t size);
    int  (*close)(struct vfs_mount *mnt, int local_fd);
    int  (*stat)(struct vfs_mount *mnt, const char *path, struct vfs_stat *st);
    int  (*mkdir)(struct vfs_mount *mnt, const char *path, uint32_t mode);
    int  (*rmdir)(struct vfs_mount *mnt, const char *path);
    int  (*unlink)(struct vfs_mount *mnt, const char *path);
    int  (*getdents)(struct vfs_mount *mnt, int local_fd, void *buf, size_t count, uint64_t offset);
};

// A mounted filesystem instance
struct vfs_mount {
    char mount_point[256];          // Absolute path where this fs is mounted (e.g. "/mnt/efi")
    struct fs_operations *ops;      // Backend filesystem operations
    void *fs_private;               // Opaque pointer to fs-specific state (e.g. fat32_fs_t*)
    uint32_t dev_id;                // Device identifier (st_dev value for stat)
    int active;                     // 1 if this slot is in use
};

// Arguments for the mount syscall (passed from userspace)
struct mount_syscall_args {
    const char *source;             // Device/source identifier (e.g. "sda1")
    const char *target;             // Mount point path (e.g. "/mnt/efi")
    const char *fstype;             // Filesystem type string (e.g. "fat32")
    uint32_t flags;                 // Mount flags (reserved for future use)
};

#define MAX_MOUNTS 16               // Maximum number of mounted filesystems
#define MOUNT_RAMDISK 0             // st_dev value for the ramdisk (default)
#define MOUNT_FLAG_NONE 0

struct vfs_stat {
    uint32_t        st_dev;     /* 0 = virt, 1 = FAT32 */
    uint32_t        st_ino;     /* Inode number */
    uint32_t        st_mode;    /* Type and permissions */
    uint32_t        st_nlink;   /* Hard links */
    uint32_t        st_uid;     /* Owner UID */
    uint32_t        st_gid;     /* Owner GID */
    uint32_t        st_rdev;    /* Device ID (Hardcoded 0) */
    
    int64_t         st_size;    /* Upgraded to 64-bit! Supports files > 2GB */
    
    uint32_t        st_blksize; /* I/O hint (4096 or cluster size) */
    uint32_t        st_blocks;  /* Allocated 512-byte blocks */
    
    struct timespec st_atim;    /* Access time */
    struct timespec st_mtim;    /* Modification time */
    struct timespec st_ctim;    /* Status change time */
};

// --- FUNCTION PROTOTYPES ---
int init_vfs(void);
size_t vfs_file_count(void);
int vfs_open(const char *path, int flags, uint32_t mode);
int vfs_write_file(int fd, const void *data, uint64_t size);
int vfs_move_file(int fd, const char *newpath);
int vfs_create_file(const void *data, const char *path, int dlen);
int vfs_read(int fd, void *buf, size_t count, uint64_t offset);
int vfs_delete_file(const char *path);
int vfs_free_fd(int fd);
int vfs_rmdir(const char *path);
int vfs_mkdir(const char *path, uint32_t mode);
int vfs_stat(const char *path, struct vfs_stat *st);
int vfs_fstat(int fd, struct vfs_stat *st);
typedef struct file DIR;

// --- DIRECTORY STREAMING API ---
DIR *opendir(const char *path);
struct vfs_dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

// --- MOUNT API ---
int vfs_mount_fs(const char *source, const char *target, const char *fstype, uint32_t flags);
int vfs_umount_fs(const char *target);
struct vfs_mount *resolve_mount(const char *path);
int vfs_getdents(int fd, void *buf, size_t count, uint64_t offset);
int vfs_listdir(const char *path, char **buf, size_t max_len);