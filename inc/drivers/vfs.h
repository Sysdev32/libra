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
int vfs_open(const char *path);
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