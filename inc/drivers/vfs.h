// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MAX_DIR_CHILDREN 32  // Safe upper bound for files/folders inside a single directory

// The file tracking structure used in the global files array
struct vfs_file {
    char path[256];     // Fixed size local buffer! No kmalloc needed for names anymore!
    uint8_t *data;
    uint64_t size;
    uint32_t mode;
};

// Metadata storage for files and directories
struct inode {
    uint32_t ino_num;    // Unique identifier number
    uint32_t mode;       // Type (file/directory) and permissions
    uint64_t size;       // Payload size in bytes
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