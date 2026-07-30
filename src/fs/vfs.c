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

#define MAX_OPEN_FILES 32
#define MAX_VFS_FILES  128  // Set a safe maximum for your ramdisk files

// POSIX Open Flags


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
    if (dir == NULL || dir->inode == NULL) {
        return 0;
    }

    if (getuid() == 0) {
        return 1;
    }

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
    } else {
        return o_f;
    }
}

// Hardware I/O Ports for CMOS
#define CMOS_INDEX        0x70
#define CMOS_DATA         0x71
#define CMOS_REG_CENTURY  0x32

static const uint16_t days_before_month[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %b0, %w1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %w1, %b0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static uint8_t read_cmos_register(uint8_t reg) {
    outb(CMOS_INDEX, reg | 0x80); 
    return inb(CMOS_DATA);
}

static int is_update_in_progress(void) {
    return (read_cmos_register(0x0A) & 0x80);
}

static int32_t calculate_epoch_seconds(uint32_t year, uint32_t month, uint32_t day, 
                                       uint32_t hour, uint32_t minute, uint32_t second) 
{
    int32_t total_days = 0;
    month -= 1;

    for (uint32_t y = 1970; y < year; y++) {
        if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) {
            total_days += 366;
        } else {
            total_days += 365;
        }
    }

    total_days += days_before_month[month];

    if (month > 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
        total_days++;
    }

    total_days += (day - 1);
    return (total_days * 86400) + (hour * 3600) + (minute * 60) + second;
}

void rtc_get_time(struct timespec *ts) {
    while (is_update_in_progress());

    uint8_t sec     = read_cmos_register(0x00);
    uint8_t min     = read_cmos_register(0x02);
    uint8_t hr      = read_cmos_register(0x04);
    uint8_t dy      = read_cmos_register(0x07);
    uint8_t mo      = read_cmos_register(0x08);
    uint8_t yr      = read_cmos_register(0x09);
    uint8_t century = read_cmos_register(CMOS_REG_CENTURY);
    uint8_t regB    = read_cmos_register(0x0B);

    if (!(regB & 0x04)) {
        sec     = (sec & 0x0F)     + ((sec / 16) * 10);
        min     = (min & 0x0F)     + ((min / 16) * 10);
        hr      = ((hr & 0x0F) + (((hr & 0x70) / 16) * 10)) | (hr & 0x80);
        dy      = (dy & 0x0F)     + ((dy / 16) * 10);
        mo      = (mo & 0x0F)     + ((mo / 16) * 10);
        yr      = (yr & 0x0F)     + ((yr / 16) * 10);
        century = (century & 0x0F) + ((century / 16) * 10);
    }

    if (!(regB & 0x02) && (hr & 0x80)) {
        hr = ((hr & 0x7F) + 12) % 24;
    }

    uint32_t full_year = (century == 0) ? (2000 + yr) : ((century * 100) + yr);

    ts->tv_sec  = calculate_epoch_seconds(full_year, mo, dy, hr, min, sec);
    ts->tv_nsec = 0;
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
            kfree(files[i].data);
            files[i].data = NULL;
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
            if (__builtin_memcmp(child->name, component, len) == 0 && child->name[len] == '\0') {
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
        printk(LOG_ERROR, "VFS: Maximum directory child limit (%d) reached!\n", MAX_DIR_CHILDREN);
        return NULL;
    }

    struct dentry *d = kmalloc(sizeof(struct dentry));
    struct inode *in = kmalloc(sizeof(struct inode));
    if (!d || !in) {
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
    if (path == NULL || path[0] != '/') return NULL;

    struct dentry *current = root_dentry;
    const char *ptr = path + 1;

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
    if (file_count >= MAX_VFS_FILES) return -1;

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
    struct limine_file *initramfs_file = find_initramfs_module(module_request.response);
    if (initramfs_file == NULL) {
        printk(LOG_ERROR, "initramfs module pointer is NULL from Limine.\n");
        return -1;
    }
    
    struct timespec ts = {0};
    rtc_get_time(&ts);
    root_dentry = create_dentry_node("", 0, S_IFDIR | 0755, 0, 0, 1, ts.tv_sec, NULL);
    if (root_dentry == NULL) {
        printk(LOG_ERROR, "Failed to allocate root dentry node memory.\n");
        return -1;
    }
    
    const uint8_t *archive = (const uint8_t *)initramfs_file->address;
    uint64_t archive_size = initramfs_file->size;
    uint64_t offset = 0;

    while (offset + sizeof(struct cpio_newc_header) <= archive_size) {
        const struct cpio_newc_header *header = (const struct cpio_newc_header *)(archive + offset);
        
        if (memcmp(header->c_magic, "070701", 6) != 0) {
            printk(LOG_ERROR, "Invalid CPIO format signature magic mismatch.\n");
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
            printk(LOG_ERROR, "Header metrics exceed boundaries of loaded ramdisk image.\n");
            return -1;
        }

        const char *path = (const char *)(archive + name_offset);
        if (strcmp(path, ".") == 0) {
            offset = next_offset;
            continue;
        }
        if (strcmp(path, "TRAILER!!!") == 0) {
            return 0;
        }
        size_t pure_path_len = strlen(path);
        int appended_fd = append_file(path, pure_path_len + 1, archive + data_offset, file_size, mode, uid, gid, mtime);
        
        if (appended_fd < 0) {
            printk(LOG_WARNING, "append_file failed! File entry was rejected by global tracker.\n");
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
                    printk(LOG_ERROR, "Static array directory child allocation limits hit.\n");
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

    printk(LOG_ERROR, "Loop finished naturally without reaching a TRAILER!!! marker sequence.\n");
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

/**
 * Modern POSIX-compliant vfs_open
 */
int vfs_open(const char *path, int flags, uint32_t mode) {
    if (path == NULL) return -EINVAL;

    char formatted_path[256];
    memset(formatted_path, 0, 256);

    if (path[0] != '/') {
        formatted_path[0] = '/';
        strncpy(formatted_path + 1, path, 254);
    } else {
        strncpy(formatted_path, path, 255);
    }

    struct dentry *target = vfs_lookup(formatted_path);

    if (target != NULL) {
        if ((flags & O_CREAT) && (flags & O_EXCL)) {
            return -EEXIST; // File exists but O_CREAT | O_EXCL specified
        }
    } else {
        if (!(flags & O_CREAT)) {
            return -ENOENT; // File doesn't exist and O_CREAT not specified
        }

        // Create the new file
        int create_res = vfs_create_file(NULL, formatted_path, 0);
        if (create_res < 0) return create_res;

        target = vfs_lookup(formatted_path);
        if (target == NULL) return -EIO;

        // Apply specified creation mode
        if (mode != 0) {
            target->inode->mode = S_IFREG | (mode & 0777);
        }
    }

    // Allocate handle in global_fd_table
    int open_fd = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (global_fd_table[i] == NULL) {
            open_fd = i;
            break;
        }
    }

    if (open_fd == -1) return -EMFILE; // Maximum process file limit reached

    struct file *f = kmalloc(sizeof(struct file));
    if (f == NULL) return -ENOMEM;

    f->dentry = target;
    f->flags = flags;
    f->offset = (flags & O_APPEND) ? target->inode->size : 0;

    global_fd_table[open_fd] = f;

    // Truncate file data if O_TRUNC specified
    if ((flags & O_TRUNC) && ((flags & O_ACCMODE) != O_RDONLY)) {
        vfs_write_file(open_fd, "", 0);
    }

    return open_fd; 
}

struct dentry *vfs_lookup_parent(const char *path) {
    if (path == NULL || path[0] != '/') return NULL;

    struct dentry *current = root_dentry;
    const char *ptr = path + 1;
    struct dentry *parent = NULL;

    while (*ptr != '\0') {
        while (*ptr == '/') ptr++;
        if (*ptr == '\0') break;

        const char *component_start = ptr;
        while (*ptr != '\0' && *ptr != '/') ptr++;
        size_t component_len = (size_t)(ptr - component_start);

        if (component_len > 0) {
            parent = current; 
            current = find_child(current, component_start, component_len);
            if (current == NULL) {
                if (*ptr == '\0') return parent;
                return NULL; 
            }
        }
    }
    return parent;
}

struct dentry *vfs_lookup_parent_by_flat_fd(int fd) {
    if (fd < 0 || (size_t)fd >= file_count) return NULL;

    const char *path = files[fd].path; 
    struct dentry *current = root_dentry;
    const char *ptr = path;
    
    if (*ptr == '/') ptr++;

    struct dentry *parent = NULL;

    while (*ptr != '\0') {
        while (*ptr == '/') ptr++;
        if (*ptr == '\0') break;

        const char *component_start = ptr;
        while (*ptr != '\0' && *ptr != '/') ptr++;
        size_t component_len = (size_t)(ptr - component_start);

        if (component_len > 0) {
            parent = current; 
            current = find_child(current, component_start, component_len);
            if (current == NULL) {
                if (*ptr == '\0') return parent;
                return NULL; 
            }
        }
    }
    return parent;
}

int vfs_create_file(const void *data, const char *path, int dlen) {
    struct timespec ts;
    rtc_get_time(&ts);

    if (append_file(path, strlen(path) + 1, NULL, 0, S_IFREG | 0644, 0, 0, ts.tv_sec) < 0) return -1;
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
    if (fd < 0 || fd >= MAX_OPEN_FILES || global_fd_table[fd] == NULL) return -1;
    kfree(global_fd_table[fd]);
    global_fd_table[fd] = NULL;
    return 0;
}

int vfs_delete_file(const char *path) {
    struct dentry *target = vfs_lookup(path);
    
    // Clear matches from open file handles
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (global_fd_table[i] != NULL && global_fd_table[i]->dentry == target) {
            kfree(global_fd_table[i]);
            global_fd_table[i] = NULL; 
        }
    }

    // 1. Unlink from dentry tree
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
        if (target->inode) kfree(target->inode);
        if (target->name) kfree(target->name);
        kfree(target);
    }

    // 2. Remove entry from flat files[] table
    const char *lookup_path = path;
    if (lookup_path[0] == '/') lookup_path++;
    else if (lookup_path[0] == '.' && lookup_path[1] == '/') lookup_path += 2;

    for (size_t i = 0; i < file_count; i++) {
        const char *stored_path = files[i].path;
        if (stored_path[0] == '/') stored_path++;
        else if (stored_path[0] == '.' && stored_path[1] == '/') stored_path += 2;

        if (strcmp(stored_path, lookup_path) == 0) {
                kfree(files[i].data);
            for (size_t j = i; j < file_count - 1; j++) {
                files[j] = files[j + 1];
            }
            file_count--;
            break;
        }
    }

    return 0;
}

/**
 * vfs_write_file
 * Atomic Delete-and-Recreate strategy that captures, updates, and restores metadata.
 */
int vfs_write_file(int fd, const void *data, uint64_t size) {
    if (fd < 0 || (size != 0 && data == NULL)) {
        return -EBADF;
    }

    char file_path[256];
    memset(file_path, 0, 256);

    uint8_t *payload_buffer = (uint8_t *)data;
    uint64_t write_size = size;

    // Metadata preservation struct
    struct {
        uint32_t mode;
        uint32_t uid;
        uint32_t gid;
        uint32_t nlink;
        struct timespec st_atim;
        struct timespec st_mtim;
        struct timespec st_ctim;
    } saved_meta;

    memset(&saved_meta, 0, sizeof(saved_meta));

    // Step 1: Check permissions & locate metadata/path
    if (fd < MAX_OPEN_FILES && global_fd_table[fd] != NULL) {
        struct file *f = global_fd_table[fd];
        if (f->dentry == NULL || f->dentry->inode == NULL) return -EBADF;
        if ((f->flags & O_ACCMODE) == O_RDONLY) return -EACCES;
        if (!perms(fd, 1)) return -EACCES;

        // Extract path from flat files[] array matching this dentry
        for (size_t i = 0; i < file_count; i++) {
            if (vfs_lookup(files[i].path) == f->dentry) {
                strncpy(file_path, files[i].path, 255);
                break;
            }
        }
        
        saved_meta.mode = f->dentry->inode->mode;
        saved_meta.uid  = f->dentry->inode->uid;
        saved_meta.gid  = f->dentry->inode->gid;
        saved_meta.nlink = f->dentry->inode->links;
        saved_meta.st_atim = f->dentry->inode->st_atim;
        saved_meta.st_mtim = f->dentry->inode->st_mtim;
        saved_meta.st_ctim = f->dentry->inode->st_ctim;

        // Process Append logic (O_APPEND)
        if ((f->flags & O_APPEND) && f->dentry->inode->size > 0) {
            write_size = f->dentry->inode->size + size;
            payload_buffer = kmalloc((size_t)write_size);
            if (!payload_buffer) return -ENOMEM;

            memcpy(payload_buffer, f->dentry->inode->data, f->dentry->inode->size);
            memcpy(payload_buffer + f->dentry->inode->size, data, size);
        }

    } else if ((size_t)fd < file_count) {
        if (!perms(fd, 1)) return -EACCES;

        strncpy(file_path, files[fd].path, 255);

        saved_meta.mode = files[fd].mode;
        saved_meta.uid  = files[fd].uid;
        saved_meta.gid  = files[fd].gid;
        saved_meta.nlink = files[fd].nlink;
        saved_meta.st_atim = files[fd].st_atim;
        saved_meta.st_mtim = files[fd].st_mtim;
        saved_meta.st_ctim = files[fd].st_ctim;
    } else {
        return -EBADF;
    }

    if (file_path[0] == '\0') {
        if (payload_buffer != data) kfree(payload_buffer);
        return -ENOENT;
    }

    // Step 2: Update modification and change timestamps
    struct timespec current_ts;
    rtc_get_time(&current_ts);
    saved_meta.st_mtim = current_ts;
    saved_meta.st_ctim = current_ts;

    // Step 3: Delete the file via vfs_delete_file
    vfs_delete_file(file_path);

    // Step 4: Recreate the file with the new data via vfs_create_file
    int new_fd = vfs_create_file(payload_buffer, file_path, (int)write_size);
    if (payload_buffer != data) kfree(payload_buffer);

    if (new_fd < 0) {
        return -1;
    }

    // Step 5: Restore and update preserved metadata across both storage models
    if ((size_t)new_fd < file_count) {
        files[new_fd].mode = saved_meta.mode;
        files[new_fd].uid  = saved_meta.uid;
        files[new_fd].gid  = saved_meta.gid;
        files[new_fd].nlink = saved_meta.nlink;
        files[new_fd].st_atim = saved_meta.st_atim;
        files[new_fd].st_mtim = saved_meta.st_mtim;
        files[new_fd].st_ctim = saved_meta.st_ctim;
    }

    struct dentry *new_dentry = vfs_lookup(file_path);
    if (new_dentry != NULL && new_dentry->inode != NULL) {
        new_dentry->inode->mode = saved_meta.mode;
        new_dentry->inode->uid  = saved_meta.uid;
        new_dentry->inode->gid  = saved_meta.gid;
        new_dentry->inode->links = saved_meta.nlink;
        new_dentry->inode->st_atim = saved_meta.st_atim;
        new_dentry->inode->st_mtim = saved_meta.st_mtim;
        new_dentry->inode->st_ctim = saved_meta.st_ctim;
    }

    // If writing through an active open file table descriptor handle, point it to the new dentry and update offset
    if (fd < MAX_OPEN_FILES && global_fd_table[fd] != NULL) {
        global_fd_table[fd]->dentry = new_dentry;
        global_fd_table[fd]->offset += size;
    }

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
    if (path == NULL || *path != '/') return -1;

    struct dentry *current_dir = root_dentry;
    const char *ptr = path + 1;

    while (*ptr != '\0') {
        while (*ptr == '/') ptr++;
        if (*ptr == '\0') break;

        const char *component_start = ptr;
        while (*ptr != '\0' && *ptr != '/') ptr++;
        size_t component_len = (size_t)(ptr - component_start);

        if (!permdir(current_dir, 2)) {
            return -EACCES;
        }

        struct dentry *next_node = find_child(current_dir, component_start, component_len);

        if (next_node == NULL) {
            if (*ptr == '\0') {
                if (current_dir->inode && (current_dir->inode->mode & S_IFMT) != S_IFDIR) {
                    return -1;
                }
                
                if (!permdir(current_dir, 1)) {
                    return -EACCES;
                }

                struct timespec ts;
                rtc_get_time(&ts);
                
                next_node = create_dentry_node(component_start, component_len, S_IFDIR | (mode & 0777), 0, 0, 1, ts.tv_sec, current_dir);
                if (next_node == NULL) return -1;
                return 0;
            } else {
                return -1;
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

    if (!permdir(parent, 1) || !permdir(parent, 2)) {
        return -EACCES;
    }

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
    printk(LOG_TRACE, "%x mode\n", inode->mode);
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