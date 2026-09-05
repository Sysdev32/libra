// SPDX-License-Identifier: GPL-3.0-only
#include <drivers/alloc.h>
#include <drivers/fb.h>
#include <fs/vfs.h>
#include <fs/fat32.h>
#include <limine.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <arch/x86_64/schedule.h>
#include <fcntl.h>

#include "hals/rtc.h"

#define MAX_OPEN_FILES 32
#define MAX_VFS_FILES  384  // Set a safe maximum for your ramdisk files

// The array that securely holds the raw VFS file pointers
static struct file *global_fd_table[MAX_OPEN_FILES];

static struct vfs_file files[MAX_VFS_FILES]; // Static array (no krealloc needed!)
static size_t file_count = 0;

// --- Mount table ---
static struct vfs_mount mount_table[MAX_MOUNTS] __attribute__((unused));
static uint32_t next_dev_id __attribute__((unused)) = 1;  // dev_id 0 = ramdisk, start counting from 1

int perms(int fd, int flag) {
    if (fd < 0) return 0;

    // Check if fd corresponds to an allocated file handle in global_fd_table first
    if (fd < MAX_OPEN_FILES && global_fd_table[fd] != NULL) {
        if (getuid() == 0) return 1;
        if (global_fd_table[fd]->dentry && global_fd_table[fd]->dentry->inode) {
            return permdir(global_fd_table[fd]->dentry, flag);
        }
    }

    // Fallback to checking the flat files array index
    if ((size_t)fd >= file_count) return 0;

    int bypass = 0;
    if (getuid() == 0) {
        bypass = 1;
    } else {
        uint32_t permissions = files[fd].mode & 00777;    // Extracts just the 9-bit octal triplets (e.g., 0755)
        uint32_t u = (permissions >> 6) & 7;
        uint32_t g = (permissions >> 3) & 7;
        uint32_t o =  permissions       & 7;

        int u_r = (u >> 2) & 1; int u_w = (u >> 1) & 1; int u_x = u & 1;
        int g_r = (g >> 2) & 1; int g_w = (g >> 1) & 1; int g_x = g & 1;
        int o_r = (o >> 2) & 1; int o_w = (o >> 1) & 1; int o_x = o & 1;

        int u_f = 0, g_f = 0, o_f = 0;
        if (flag == 0) {
            u_f = u_r; g_f = g_r; o_f = o_r;
        } else if (flag == 1) {
            u_f = u_w; g_f = g_w; o_f = o_w;
        } else if (flag == 2) {
            u_f = u_x; g_f = g_x; o_f = o_x;
        }

        if ((uint32_t)getuid() == files[fd].uid) {
            if (u_f) bypass = 1;
        } else if ((uint32_t)getgid() == files[fd].gid) {
            if (g_f) bypass = 1;
        } else {
            if (o_f) bypass = 1;
        }
    }
    return bypass;
}

int permdir(struct dentry *dir, int flag) {
    if (dir == NULL || dir->inode == NULL) return 0;
    if (getuid() == 0) return 1;

    uint32_t permissions = dir->inode->mode & 00777;
    uint32_t u = (permissions >> 6) & 7;
    uint32_t g = (permissions >> 3) & 7;
    uint32_t o =  permissions       & 7;

    int u_f = 0, g_f = 0, o_f = 0;

    if (flag == 0) {
        u_f = (u >> 2) & 1;
        g_f = (g >> 2) & 1;
        o_f = (o >> 2) & 1;
    } else if (flag == 1) {
        u_f = (u >> 1) & 1;
        g_f = (g >> 1) & 1;
        o_f = (o >> 1) & 1;
    } else if (flag == 2) {
        u_f =  u       & 1;
        g_f =  g       & 1;
        o_f =  o       & 1;
    }

    if ((uint32_t)getuid() == dir->inode->uid) {
        return u_f;
    } else if ((uint32_t)getgid() == dir->inode->gid) {
        return g_f;
    }
    return o_f;
}

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0
};

struct __attribute__((packed)) cpio_newc_header {
    char c_magic[6];
    char c_ino[8];
    char c_mode[8];
    char c_uid[8];
    char c_gid[8];
    char c_nlink[8];
    char c_mtime[8];
    char c_filesize[8];
    char c_devmajor[8];
    char c_devminor[8];
    char c_rdevmajor[8];
    char c_rdevminor[8];
    char c_namesize[8];
    char c_check[8];
};

static uint64_t align4(uint64_t value) {
    return (value + 3) & ~3ULL;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex8(const char text[8], uint64_t *out) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; i++) {
        int digit = hex_digit(text[i]);
        if (digit < 0) return -1;
        value = (value << 4) | (uint64_t)digit;
    }
    *out = value;
    return 0;
}

static void clear_files(void) __attribute__((unused));
static void clear_files(void) {
    for (size_t i = 0; i < file_count; i++) {
        memset(files[i].path, 0, 256);
        if (files[i].data) {
            kfree(files[i].data);
            files[i].data = NULL;
        }
        files[i].size = 0;
        files[i].mode = 0;
        files[i].uid = 0;
        files[i].gid = 0;
    }
    file_count = 0;
}

struct dentry *find_child(struct dentry *parent, const char *component, size_t len) {
    if (parent == NULL || component == NULL || len == 0) return NULL;

    for (size_t i = 0; i < parent->child_count; i++) {
        struct dentry *child = parent->children[i];
        if (child != NULL && child->name != NULL) {
            if (memcmp(child->name, component, len) == 0 && child->name[len] == '\0') {
                return child;
            }
        }
    }
    return NULL;
}

static struct limine_file *find_initramfs_module(struct limine_module_response *response) {
    if (response == NULL) return NULL;
    for (uint64_t i = 0; i < response->module_count; i++) {
        struct limine_file *file = response->modules[i];
        if (file != NULL && file->string != NULL && strcmp(file->string, "initramfs") == 0) {
            return file;
        }
    }
    return NULL;
}

#define S_IFMT   0xF000
#define S_IFDIR  0x4000
#define S_IFREG  0x8000

static struct dentry *root_dentry = NULL;
uint32_t global_ino_counter = 1;

static struct dentry *create_dentry_node(const char *name, size_t name_len, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t nlink, uint64_t mtime, struct dentry *parent) {
    if (parent != NULL && parent->child_count >= MAX_DIR_CHILDREN) {
        printk(LOG_ERROR, "[VFS] create_dentry_node: Maximum directory child limit reached\n");
        return NULL;
    }

    struct dentry *d = kmalloc(sizeof(struct dentry));
    struct inode *in = kmalloc(sizeof(struct inode));
    if (!d || !in) {
        printk(LOG_ERROR, "[VFS] create_dentry_node: kmalloc failed\n");
        if (d) kfree(d);
        if (in) kfree(in);
        return NULL;
    }

    in->ino_num = global_ino_counter++;
    in->mode = mode;
    in->uid = uid;
    in->gid = gid;
    in->links = nlink;
    in->size = 0;
    in->data = NULL;
    in->st_atim.tv_sec = mtime;
    in->st_atim.tv_nsec = 0;
    in->st_mtim.tv_sec = mtime;
    in->st_mtim.tv_nsec = 0;
    in->st_ctim.tv_sec = mtime;
    in->st_ctim.tv_nsec = 0;

    d->name = kmalloc(name_len + 1);
    if (!d->name) {
        kfree(in);
        kfree(d);
        return NULL;
    }
    memcpy(d->name, name, name_len);
    d->name[name_len] = '\0';

    d->inode = in;
    d->parent = parent;
    d->child_count = 0;

    for (size_t i = 0; i < MAX_DIR_CHILDREN; i++) {
        d->children[i] = NULL;
    }

    if (parent) {
        parent->children[parent->child_count] = d;
        parent->child_count++;
    }

    return d;
}

struct dentry *vfs_lookup(const char *path) {
    if (path == NULL) return NULL;

    struct dentry *current = root_dentry;
    const char *ptr = path;
    if (*ptr == '/') ptr++;

    while (*ptr != '\0') {
        while (*ptr == '/') ptr++;
        if (*ptr == '\0') break;

        const char *component_start = ptr;
        while (*ptr != '\0' && *ptr != '/') ptr++;
        size_t component_len = (size_t)(ptr - component_start);

        if (component_len > 0) {
            current = find_child(current, component_start, component_len);
            if (current == NULL) return NULL;
        }
    }
    return current;
}

static int append_file(const char *path, uint64_t path_size, const uint8_t *data, uint64_t size, uint32_t mode, uint32_t uid, uint32_t gid, uint64_t mtime) {
    if (path == NULL || path_size == 0) return -1;
    if (file_count >= MAX_VFS_FILES) {
        printk(LOG_ERROR, "[VFS] append_file: table full\n");
        return -1;
    }

    memset(files[file_count].path, 0, 256);

    size_t copy_len = (size_t)path_size;
    if (copy_len >= 256) {
        copy_len = 255;
    }

    memcpy(files[file_count].path, path, copy_len);
    files[file_count].path[copy_len] = '\0';

    files[file_count].data = (uint8_t *)data;
    files[file_count].size = size;
    files[file_count].mode = mode;
    files[file_count].uid = uid;
    files[file_count].gid = gid;
    files[file_count].nlink = 1;
    files[file_count].st_atim.tv_sec = mtime;
    files[file_count].st_atim.tv_nsec = 0;
    files[file_count].st_mtim.tv_sec = mtime;
    files[file_count].st_mtim.tv_nsec = 0;
    files[file_count].st_ctim.tv_sec = mtime;
    files[file_count].st_ctim.tv_nsec = 0;

    int created_fd = (int)file_count;
    file_count++;

    return created_fd;
}

int init_vfs(void) {
    printk(LOG_INFO, "[VFS] Initializing VFS...\n");
    struct limine_file *initramfs_file = find_initramfs_module(module_request.response);
    if (initramfs_file == NULL) {
        printk(LOG_ERROR, "[VFS] init_vfs: initramfs module pointer is NULL from Limine.\n");
        return -1;
    }

    struct timespec ts = {0};
    rtc_get_time(&ts);
    root_dentry = create_dentry_node("", 0, S_IFDIR | 0755, 0, 0, 1, ts.tv_sec, NULL);
    if (root_dentry == NULL) {
        printk(LOG_ERROR, "[VFS] init_vfs: Failed to allocate root dentry node memory.\n");
        return -1;
    }

    const uint8_t *archive = (const uint8_t *)initramfs_file->address;
    uint64_t archive_size = initramfs_file->size;
    uint64_t offset = 0;

    while (offset + sizeof(struct cpio_newc_header) <= archive_size) {
        const struct cpio_newc_header *header = (const struct cpio_newc_header *)(archive + offset);

        if (memcmp(header->c_magic, "070701", 6) != 0) {
            printk(LOG_ERROR, "[VFS] init_vfs: Invalid CPIO format signature magic mismatch.\n");
            return -1;
        }

        uint64_t mode = 0, file_size = 0, name_size = 0;
        uint64_t uid = 0, gid = 0, nlink = 0, mtime = 0;

        parse_hex8(header->c_mode, &mode);
        parse_hex8(header->c_filesize, &file_size);
        parse_hex8(header->c_namesize, &name_size);
        parse_hex8(header->c_uid, &uid);
        parse_hex8(header->c_gid, &gid);
        parse_hex8(header->c_nlink, &nlink);
        parse_hex8(header->c_mtime, &mtime);

        uint64_t name_offset = offset + sizeof(struct cpio_newc_header);
        uint64_t data_offset = align4(name_offset + name_size);
        uint64_t next_offset = align4(data_offset + file_size);

        if (name_offset + name_size > archive_size || data_offset + file_size > archive_size) {
            printk(LOG_ERROR, "[VFS] init_vfs: Header metrics exceed boundaries of loaded ramdisk image.\n");
            return -1;
        }

        const char *path = (const char *)(archive + name_offset);
        if (strcmp(path, ".") == 0) {
            offset = next_offset;
            continue;
        }
        if (strcmp(path, "TRAILER!!!") == 0) {
            printk(LOG_INFO, "[VFS] init_vfs: Initramfs unpacked successfully.\n");
            return 0;
        }
        printk(LOG_DEBUG, "path: %s\n", path);
        size_t pure_path_len = strlen(path);
        int appended_fd = append_file(path, pure_path_len + 1, archive + data_offset, file_size, mode, uid, gid, mtime);

        if (appended_fd < 0) {
            printk(LOG_ERROR, "[VFS] init_vfs: append_file failed! Ramdisk entry rejected.\n");
            for(;;);
        }

        struct dentry *current_dir = root_dentry;
        const char *ptr = path;
        while (*ptr != '\0') {
            while (*ptr == '/') ptr++;
            if (*ptr == '\0') break;

            const char *component_start = ptr;
            while (*ptr != '\0' && *ptr != '/') ptr++;
            size_t component_len = (size_t)(ptr - component_start);

            struct dentry *next_node = find_child(current_dir, component_start, component_len);

            if (next_node == NULL) {
                uint32_t node_mode = (*ptr == '/') ? (S_IFDIR | 0755) : (uint32_t)mode;
                struct timespec spec = {0};
                rtc_get_time(&spec);
                next_node = create_dentry_node(component_start, component_len, node_mode, uid, gid, nlink, spec.tv_sec, current_dir);
                if (next_node == NULL) {
                    printk(LOG_ERROR, "[VFS] init_vfs: Static array directory child allocation limits hit.\n");
                    return -1;
                }
                if (*ptr == '\0' && (node_mode & S_IFMT) == S_IFREG) {
                    next_node->inode->size = file_size;
                    next_node->inode->mode = mode;
                    next_node->inode->uid  = uid;
                    next_node->inode->gid  = gid;
                    next_node->inode->data = files[appended_fd].data;
                }
            }

            current_dir = next_node;
        }

        offset = next_offset;
    }

    printk(LOG_ERROR, "[VFS] init_vfs: Reached end of archive without TRAILER!!! marker.\n");
    return -1;
}

size_t vfs_file_count(void) {
    return file_count;
}

int vfs_read(int fd, void *buf, size_t count, uint64_t offset) {
    if (fd < 0 || buf == NULL) return -EBADF;

    // Standard Descriptor Read
    if (fd < MAX_OPEN_FILES && global_fd_table[fd] != NULL) {
        struct file *f = global_fd_table[fd];
        if ((f->flags & O_ACCMODE) == O_WRONLY) return -EACCES;
        if (!perms(fd, 0)) return -EACCES;

        if (f->dentry && f->dentry->inode) {
            struct inode *in = f->dentry->inode;
            uint64_t r_offset = (offset == (uint64_t)-1) ? f->offset : offset;

            if (r_offset >= in->size) return 0;
            if (r_offset + count > in->size) count = (size_t)(in->size - r_offset);

            memcpy(buf, (const uint8_t *)in->data + r_offset, count);
            if (offset == (uint64_t)-1) {
                f->offset += count;
            }
            return (int)count;
        }
    }

    // Flat file read fallback
    if ((size_t)fd < file_count) {
        if (!perms(fd, 0)) return -EACCES;
        struct vfs_file *file = &files[fd];
        if (offset >= file->size) return 0;
        if (offset + count > file->size) {
            count = (size_t)(file->size - offset);
        }

        memcpy(buf, (const uint8_t *)file->data + offset, count);
        return (int)count;
    }

    return -EBADF;
}

int vfs_open(const char *path, int flags, uint32_t mode) {
    if (path == NULL) return -EINVAL;

    // 1. Resolve target
    struct dentry *target = vfs_lookup(path);

    if (target != NULL) {
        if ((flags & O_CREAT) && (flags & O_EXCL)) {
            return -EEXIST;
        }
    } else {
        // Must have O_CREAT flag set to create missing target
        if (!(flags & O_CREAT)) {
            return -ENOENT;
        }

        // Create the file
        int create_res = vfs_create_file(NULL, path, 0);
        printk(LOG_INFO, "create_res=%d\n", create_res);
        if (create_res < 0) return create_res;

        // Re-lookup the newly created dentry
        target = vfs_lookup(path);
        if (target == NULL) return -EIO;

        if (mode != 0) {
            target->inode->mode = S_IFREG | (mode & 0777);
        }
    }

    // Allocate open file descriptor slot
    int open_fd = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (global_fd_table[i] == NULL) {
            open_fd = i;
            break;
        }
    }

    if (open_fd == -1) return -EMFILE;

    struct file *f = kmalloc(sizeof(struct file));
    if (f == NULL) return -ENOMEM;

    f->dentry = target;
    f->flags = flags;
    f->offset = (flags & O_APPEND) ? target->inode->size : 0;

    global_fd_table[open_fd] = f;

    return open_fd;
}

struct dentry *vfs_lookup_parent(const char *path) {
    if (path == NULL) return NULL;

    struct dentry *current = root_dentry;
    const char *ptr = path;
    if (*ptr == '/') ptr++;

    struct dentry *parent = root_dentry;

    while (*ptr != '\0') {
        while (*ptr == '/') ptr++;
        if (*ptr == '\0') break;

        const char *component_start = ptr;
        while (*ptr != '\0' && *ptr != '/') ptr++;
        size_t component_len = (size_t)(ptr - component_start);

        if (component_len > 0) {
            struct dentry *next = find_child(current, component_start, component_len);
            if (next == NULL) {
                const char *peek = ptr;
                while (*peek == '/') peek++;
                if (*peek == '\0') return current;
                return NULL;
            }
            parent = current;
            current = next;
        }
    }
    return parent;
}

struct dentry *vfs_lookup_parent_by_flat_fd(int fd) {
    if (fd < 0 || (size_t)fd >= file_count) return NULL;
    return vfs_lookup_parent(files[fd].path);
}

int vfs_create_file(const void *data, const char *path, int dlen) {
    struct timespec ts;
    rtc_get_time(&ts);

    if (append_file(path, strlen(path) + 1, NULL, 0, S_IFREG | 0644, 0, 0, ts.tv_sec) < 0) {
        return -1;
    }
    int fd = (int)(file_count - 1);

    struct dentry *parent = vfs_lookup_parent(path);
    if (parent == NULL) {
        parent = root_dentry;
    }

    const char *filename = path;
    const char *last_slash = strrchr(path, '/');
    if (last_slash != NULL) {
        filename = last_slash + 1;
    }

    struct dentry *child = find_child(parent, filename, strlen(filename));
    if (child == NULL) {
        child = create_dentry_node(filename, strlen(filename), S_IFREG | 0644, 0, 0, 1, ts.tv_sec, parent);
    }

    if (data != NULL && dlen > 0) {
        uint8_t *allocated_data = kmalloc((size_t)dlen);
        if (allocated_data != NULL) {
            memcpy(allocated_data, data, (size_t)dlen);
            files[fd].data = allocated_data;
            files[fd].size = (uint64_t)dlen;

            if (child != NULL && child->inode != NULL) {
                child->inode->data = allocated_data;
                child->inode->size = (uint64_t)dlen;
            }
        }
    } else if (child != NULL && child->inode != NULL) {
        child->inode->data = files[fd].data;
        child->inode->size = files[fd].size;
    }

    return fd;
}

int vfs_free_fd(int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || global_fd_table[fd] == NULL) {
        return -1;
    }
    kfree(global_fd_table[fd]);
    global_fd_table[fd] = NULL;
    return 0;
}

int vfs_delete_file(const char *path) {
    struct dentry *target = vfs_lookup(path);

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (global_fd_table[i] != NULL && global_fd_table[i]->dentry == target) {
            kfree(global_fd_table[i]);
            global_fd_table[i] = NULL;
        }
    }

    if (target != NULL && target->parent != NULL) {
        struct dentry *parent = target->parent;
        for (size_t i = 0; i < parent->child_count; i++) {
            if (parent->children[i] == target) {
                for (size_t j = i; j < parent->child_count - 1; j++) {
                    parent->children[j] = parent->children[j + 1];
                }
                parent->children[parent->child_count - 1] = NULL;
                parent->child_count--;
                break;
            }
        }
        if (target->inode) {
            if (target->inode->data) kfree(target->inode->data);
            kfree(target->inode);
        }
        if (target->name) kfree(target->name);
        kfree(target);
    }

    const char *lookup_path = path;
    if (lookup_path[0] == '/') lookup_path++;
    else if (lookup_path[0] == '.' && lookup_path[1] == '/') lookup_path += 2;

    for (size_t i = 0; i < file_count; i++) {
        const char *stored_path = files[i].path;
        if (stored_path[0] == '/') stored_path++;
        else if (stored_path[0] == '.' && stored_path[1] == '/') stored_path += 2;

        if (strcmp(stored_path, lookup_path) == 0) {
            for (size_t j = i; j < file_count - 1; j++) {
                files[j] = files[j + 1];
            }
            file_count--;
            break;
        }
    }

    return 0;
}

int vfs_write_file(int fd, const void *data, uint64_t size) {
    if (fd < 0 || (size != 0 && data == NULL)) return -EBADF;

    struct inode *in = NULL;

    if (fd < MAX_OPEN_FILES && global_fd_table[fd] != NULL) {
        struct file *f = global_fd_table[fd];
        if (f->dentry == NULL || f->dentry->inode == NULL) return -EBADF;
        in = f->dentry->inode;
    } else if ((size_t)fd < file_count) {
        struct dentry *d = vfs_lookup(files[fd].path);
        if (d != NULL && d->inode != NULL) {
            in = d->inode;
        }
    }

    if (in == NULL) return -ENOENT;

    if (fd < MAX_OPEN_FILES && global_fd_table[fd] != NULL && (global_fd_table[fd]->flags & O_APPEND)) {
        uint64_t new_size = in->size + size;
        uint8_t *new_data = kmalloc((size_t)new_size);
        if (!new_data) return -ENOMEM;

        if (in->data && in->size > 0) {
            memcpy(new_data, in->data, in->size);
            kfree(in->data);
        }
        memcpy(new_data + in->size, data, size);

        in->data = new_data;
        in->size = new_size;
        global_fd_table[fd]->offset = new_size;
    } else {
        if (in->data) {
            kfree(in->data);
            in->data = NULL;
        }

        if (size > 0) {
            in->data = kmalloc((size_t)size);
            if (!in->data) return -ENOMEM;
            memcpy(in->data, data, (size_t)size);
        }

        in->size = size;
        if (fd < MAX_OPEN_FILES && global_fd_table[fd] != NULL) {
            global_fd_table[fd]->offset = size;
        }
    }

    struct timespec current_ts;
    rtc_get_time(&current_ts);
    in->st_mtim = current_ts;
    in->st_ctim = current_ts;

    return (int)size;
}

int vfs_move_file(int fd, const char *newpath) {
    if (fd < 0 || (size_t)fd >= file_count || newpath == NULL) {
        return -1;
    }

    struct dentry *old_parent = vfs_lookup_parent_by_flat_fd(fd);
    struct dentry *new_parent = vfs_lookup_parent(newpath);

    if (!permdir(old_parent, 1) || !permdir(old_parent, 2) ||
        !permdir(new_parent, 1) || !permdir(new_parent, 2)) {
        return -EACCES;
    }

    memset(files[fd].path, 0, 256);
    strncpy(files[fd].path, newpath, 255);
    files[fd].path[255] = '\0';

    return 0;
}

int vfs_mkdir(const char *path, uint32_t mode) {
    if (path == NULL) return -1; // return

    struct dentry *current_dir = root_dentry;
    const char *ptr = path;
    if (*ptr == '/') ptr++;

    while (*ptr != '\0') {
        while (*ptr == '/') ptr++;
        if (*ptr == '\0') break;

        const char *component_start = ptr;
        while (*ptr != '\0' && *ptr != '/') ptr++;
        size_t component_len = (size_t)(ptr - component_start);

        if (!permdir(current_dir, 2)) return -EACCES; // return

        struct dentry *next_node = find_child(current_dir, component_start, component_len);

        if (next_node == NULL) {
            if (*ptr == '\0') {
                if (current_dir->inode && (current_dir->inode->mode & S_IFMT) != S_IFDIR) return -1; // also a return
                if (!permdir(current_dir, 1)) return -EACCES; // a return

                struct timespec ts;
                rtc_get_time(&ts);

                next_node = create_dentry_node(component_start, component_len, S_IFDIR | (mode & 0777), 0, 0, 1, ts.tv_sec, current_dir);
                if (next_node == NULL) return -1; // return
                return 0; // return
            } else {
                return -1; // return
            }
        }
        current_dir = next_node;
    }
    return -1;
}

int vfs_rmdir(const char *path) {
    struct dentry *target = vfs_lookup(path);
    if (target == NULL || target->parent == NULL) return -1;
    if (target->inode == NULL || (target->inode->mode & S_IFMT) != S_IFDIR) return -1;
    if (target->child_count > 0) return -1;

    struct dentry *parent = target->parent;

    if (!permdir(parent, 1) || !permdir(parent, 2)) return -EACCES;

    for (size_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == target) {
            for (size_t j = i; j < parent->child_count - 1; j++) {
                parent->children[j] = parent->children[j + 1];
            }
            parent->children[parent->child_count - 1] = NULL;
            parent->child_count--;
            break;
        }
    }

    if (target->inode) kfree(target->inode);
    if (target->name) kfree(target->name);
    kfree(target);
    return 0;
}

int vfs_stat(const char *path, struct vfs_stat *st) {
    if (path == NULL || st == NULL) return -1;

    struct dentry *d = vfs_lookup(path);
    if (d == NULL || d->inode == NULL) return -1;

    st->st_ino   = d->inode->ino_num;
    st->st_mode  = d->inode->mode;
    st->st_uid   = d->inode->uid;
    st->st_gid   = d->inode->gid;
    st->st_size  = d->inode->size;
    st->st_nlink = d->inode->links;
    st->st_ctim  = d->inode->st_ctim;
    st->st_mtim  = d->inode->st_mtim;
    st->st_atim  = d->inode->st_atim;

    return 0;
}

int vfs_fstat(int fd, struct vfs_stat *st) {
    if (st == NULL) return -2;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -3;

    struct file *file = global_fd_table[fd];

    if (file == NULL) {
        if ((size_t)fd < vfs_file_count()) {
            struct vfs_file *vf = &files[fd];

            st->st_ino     = (uint32_t)(fd + 100);
            st->st_mode    = vf->mode;
            st->st_uid     = vf->uid;
            st->st_gid     = vf->gid;
            st->st_size    = vf->size;
            st->st_blksize = 512;
            st->st_blocks  = (uint32_t)((vf->size + 511) >> 9);
            st->st_dev     = 0;
            st->st_rdev    = 0;
            st->st_nlink   = vf->nlink;
            st->st_ctim    = vf->st_ctim;
            st->st_mtim    = vf->st_mtim;
            st->st_atim    = vf->st_atim;
            return 0;
        }
        return -4;
    }

    struct inode *inode = file->dentry->inode;
    if (inode == NULL) return -5;

    st->st_ino     = inode->ino_num;
    st->st_mode    = inode->mode;
    st->st_uid     = inode->uid;
    st->st_gid     = inode->gid;
    st->st_size    = inode->size;
    st->st_blksize = 512;
    st->st_blocks  = (uint32_t)((inode->size + 511) >> 9);
    st->st_dev     = 0;
    st->st_rdev    = 0;
    st->st_nlink   = inode->links;
    st->st_ctim    = inode->st_ctim;
    st->st_mtim    = inode->st_mtim;
    st->st_atim    = inode->st_atim;
    return 0;
}

int vfs_getdents(int fd, void *buf, size_t count, uint64_t offset) {
    if (buf == NULL || count < sizeof(struct vfs_dirent)) return -EINVAL;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return -EBADF;

    if (perms(fd, 0)) {
        struct file *file = global_fd_table[fd];
        if (file == NULL || file->dentry == NULL || file->dentry->inode == NULL) return -EBADF;

        struct dentry *dir_dentry = file->dentry;
        if ((dir_dentry->inode->mode & S_IFMT) != S_IFDIR) return -ENOTDIR;

        uint8_t *user_buf = (uint8_t *)buf;
        size_t bytes_written = 0;
        uint64_t child_idx = offset;

        while (child_idx < dir_dentry->child_count) {
            struct dentry *child = dir_dentry->children[child_idx];
            if (child == NULL || child->inode == NULL || child->name == NULL) {
                child_idx++;
                continue;
            }

            if (bytes_written + sizeof(struct vfs_dirent) > count) {
                if (bytes_written == 0) return -EINVAL;
                break;
            }

            struct vfs_dirent *vd = (struct vfs_dirent *)(user_buf + bytes_written);
            vd->d_ino = child->inode->ino_num;
            vd->d_type = ((child->inode->mode & S_IFMT) == S_IFDIR) ? 4 : 8;

            memset(vd->d_name, 0, 256);
            strncpy(vd->d_name, child->name, 255);
            vd->d_name[255] = '\0';

            bytes_written += sizeof(struct vfs_dirent);
            child_idx++;
        }

        return (int)bytes_written;
    }
    return -EACCES;
}

int vfs_listdir(const char *path, char **buf, size_t max_len) {
    if (path == NULL || buf == NULL) return -EINVAL;

    struct dentry *dir_dentry = vfs_lookup(path);
    if (dir_dentry == NULL || dir_dentry->inode == NULL) return -ENOENT;
    if ((dir_dentry->inode->mode & S_IFMT) != S_IFDIR) return -ENOTDIR;
    if (!permdir(dir_dentry, 0)) return -EACCES;

    size_t written = 0;
    for (size_t i = 0; i < dir_dentry->child_count && written < max_len; i++) {
        struct dentry *child = dir_dentry->children[i];
        if (child == NULL || child->name == NULL) continue;
        if (buf[written] == NULL) return -EINVAL;

        strncpy(buf[written], child->name, 255);
        buf[written][255] = '\0';
        written++;
    }

    return (int)written;
}

typedef struct file DIR;

DIR *opendir(const char *path) {
    if (path == NULL) return NULL;

    struct dentry *dir_dentry = vfs_lookup(path);
    if (dir_dentry == NULL || dir_dentry->inode == NULL) return NULL;
    if ((dir_dentry->inode->mode & S_IFMT) != S_IFDIR) return NULL;
    if (!permdir(dir_dentry, 0)) return NULL;

    DIR *dir_stream = kmalloc(sizeof(DIR));
    if (dir_stream == NULL) return NULL;

    dir_stream->dentry = dir_dentry;
    dir_stream->offset = 0;
    dir_stream->flags  = 0;

    return dir_stream;
}

struct vfs_dirent *readdir(DIR *dirp) {
    if (dirp == NULL || dirp->dentry == NULL || dirp->dentry->inode == NULL) return NULL;
    if (!permdir(dirp->dentry, 0)) return NULL;

    static struct vfs_dirent entry;

    while (dirp->offset < dirp->dentry->child_count) {
        struct dentry *child = dirp->dentry->children[dirp->offset];
        dirp->offset++;

        if (child == NULL || child->inode == NULL || child->name == NULL) continue;

        entry.d_ino = child->inode->ino_num;
        entry.d_type = ((child->inode->mode & S_IFMT) == S_IFDIR) ? 4 : 8;

        memset(entry.d_name, 0, 256);
        strncpy(entry.d_name, child->name, 255);
        entry.d_name[255] = '\0';

        return &entry;
    }

    return NULL;
}

int closedir(DIR *dirp) {
    if (dirp == NULL) return -1;
    kfree(dirp);
    return 0;
}
int vfs_link(const char *oldpath, const char *newpath)
{
    if (!oldpath || !newpath)
        return -EINVAL;

    // destination must not exist
    if (vfs_lookup(newpath))
        return -EEXIST;

    struct dentry *src = vfs_lookup(oldpath);
    if (!src || !src->inode)
        return -ENOENT;

    // Don't hard-link directories.
    if ((src->inode->mode & S_IFMT) == S_IFDIR)
        return -EPERM;

    struct dentry *parent = vfs_lookup_parent(newpath);
    if (!parent)
        return -ENOENT;

    if (!permdir(parent, 1) || !permdir(parent, 2))
        return -EACCES;

    const char *name = strrchr(newpath, '/');
    name = name ? name + 1 : newpath;

    struct timespec ts;
    rtc_get_time(&ts);

    struct dentry *d = create_dentry_node(
        name,
        strlen(name),
        src->inode->mode,
        src->inode->uid,
        src->inode->gid,
        src->inode->links + 1,
        ts.tv_sec,
        parent);

    if (!d)
        return -ENOMEM;

    /* Share the SAME inode. */
    kfree(d->inode);
    d->inode = src->inode;

    src->inode->links++;

    if (file_count < MAX_VFS_FILES) {
        memset(files[file_count].path, 0, sizeof(files[file_count].path));
        strncpy(files[file_count].path, newpath, 255);

        files[file_count].data   = src->inode->data;
        files[file_count].size   = src->inode->size;
        files[file_count].mode   = src->inode->mode;
        files[file_count].uid    = src->inode->uid;
        files[file_count].gid    = src->inode->gid;
        files[file_count].nlink  = src->inode->links;
        files[file_count].st_atim = src->inode->st_atim;
        files[file_count].st_mtim = src->inode->st_mtim;
        files[file_count].st_ctim = src->inode->st_ctim;

        file_count++;
    }

    return 0;
}