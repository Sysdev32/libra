// SPDX-License-Identifier: GPL-3.0-only
#include <drivers/alloc.h>
#include <drivers/fb.h>
#include <drivers/vfs.h>
#include <drivers/fat32.h>
#include <limine.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <arch/x86_64/schedule.h>
#define MAX_OPEN_FILES 32
#define MAX_VFS_FILES  128  // Set a safe maximum for your ramdisk files

// The array that securely holds the raw VFS file pointers
static struct file *global_fd_table[MAX_OPEN_FILES];

static struct vfs_file files[MAX_VFS_FILES]; // Static array (no krealloc needed!)
static size_t file_count = 0;

// --- Mount table ---
static struct vfs_mount mount_table[MAX_MOUNTS];
static uint32_t next_dev_id = 1;  // dev_id 0 = ramdisk, start counting from 1
int perms(int fd, int flag) {
    int bypass = 0;
    if (getuid() == 0) {
        bypass = 1;
    } else {
        uint32_t permissions = files[fd].mode & 00777;    // Extracts just the 9-bit octal triplets (e.g., 0755)
        // Isolate the three triplets
        uint32_t u = (permissions >> 6) & 7;
        uint32_t g = (permissions >> 3) & 7;
        uint32_t o =  permissions       & 7;

        // Split Owner bits
        int u_r = (u >> 2) & 1; int u_w = (u >> 1) & 1; int u_x = u & 1;

        // Split Group bits
        int g_r = (g >> 2) & 1; int g_w = (g >> 1) & 1; int g_x = g & 1;

        // Split Other bits
        int o_r = (o >> 2) & 1; int o_w = (o >> 1) & 1; int o_x = o & 1;
        int u_f = 0;
        int g_f = 0;
        int o_f = 0;
        if (flag == 0) {
            u_f = u_r;
            g_f = g_r;
            o_f = o_r;
        } else if (flag == 1) {
            u_f = u_w;
            g_f = g_w;
            o_f = o_w;
        } else if (flag == 2) {
            u_f = u_x;
            g_f = g_x;
            o_f = o_x;
        }
        if (getuid() == files[fd].uid) { 
            if (u_f) {
                bypass = 1;
            }
        } else if (getgid() == files[fd].gid) {
            if (g_f) {
                bypass = 1;
            }
        } else {
            if (o_f) {
                bypass = 1;
            }
        }
    }
    return bypass;
}
int permdir(struct dentry *dir, int flag) {
    // Rule 1: If the directory node or its underlying inode is null, reject access
    if (dir == NULL || dir->inode == NULL) {
        return 0;
    }

    // Rule 2: Root (UID 0) always skips standard permission tracking
    if (getuid() == 0) {
        return 1;
    }

    // Isolate the 9-bit permission triplet mask
    uint32_t permissions = dir->inode->mode & 00777;
    
    uint32_t u = (permissions >> 6) & 7;
    uint32_t g = (permissions >> 3) & 7;
    uint32_t o =  permissions       & 7;

    int u_f = 0;
    int g_f = 0;
    int o_f = 0;

    // Isolate the requested permission bit type (0 = Read, 1 = Write, 2 = Execute)
    if (flag == 0) {
        u_f = (u >> 2) & 1; // Owner read
        g_f = (g >> 2) & 1; // Group read
        o_f = (o >> 2) & 1; // Other read
    } else if (flag == 1) {
        u_f = (u >> 1) & 1; // Owner write
        g_f = (g >> 1) & 1; // Group write
        o_f = (o >> 1) & 1; // Other write
    } else if (flag == 2) {
        u_f =  u       & 1; // Owner execute (Traverse / Search)
        g_f =  g       & 1; // Group execute
        o_f =  o       & 1; // Other execute
    }

    // Step through priority matching logic (First match wins)
    if (getuid() == dir->inode->uid) { 
        return u_f;
    } else if (getgid() == dir->inode->gid) {
        return g_f;
    } else {
        return o_f;
    }
}
// Hardware I/O Ports for CMOS
#define CMOS_INDEX        0x70
#define CMOS_DATA         0x71

// ACPI Standard Century Register location
#define CMOS_REG_CENTURY  0x32

// Cumulative days in a standard year before the start of each month (0-indexed)
static const uint16_t days_before_month[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

// Inline assembly wrappers for x86 port I/O
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %b0, %w1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %w1, %b0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Low-level CMOS register reader
static uint8_t read_cmos_register(uint8_t reg) {
    // Bit 7 set to 0x80 disables Non-Maskable Interrupts (NMI) during the read
    outb(CMOS_INDEX, reg | 0x80); 
    return inb(CMOS_DATA);
}

// Checks if the RTC chip is in the middle of a 1-second update cycle
static int is_update_in_progress(void) {
    return (read_cmos_register(0x0A) & 0x80);
}

// Formulates the total Unix epoch seconds from calendar values (32-bit safe math)
static int32_t calculate_epoch_seconds(uint32_t year, uint32_t month, uint32_t day, 
                                       uint32_t hour, uint32_t minute, uint32_t second) 
{
    int32_t total_days = 0;
    month -= 1; // Convert 1-12 calendar format to 0-11 array index

    // Step 1: Add up days for completed years since 1970
    for (uint32_t y = 1970; y < year; y++) {
        if ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) {
            total_days += 366; // Leap year
        } else {
            total_days += 365; // Normal year
        }
    }

    // Step 2: Add days of completed months in the current year
    total_days += days_before_month[month];

    // Add 1 day if we passed February in a current leap year
    if (month > 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
        total_days++;
    }

    // Step 3: Add days passed in the current month (day is 1-indexed, subtract 1)
    total_days += (day - 1);

    // Step 4: Scale days, hours, and minutes down to absolute seconds
    return (total_days * 86400) + (hour * 3600) + (minute * 60) + second;
}

/**
 * Public Driver Function: rtc_get_time
 * Populates your timespec structure directly with current hardware time.
 */
void rtc_get_time(struct timespec *ts) {
    // Prevent reading scrambled data during a clock update tick
    while (is_update_in_progress());

    // Read raw values from the CMOS registers
    uint8_t sec     = read_cmos_register(0x00);
    uint8_t min     = read_cmos_register(0x02);
    uint8_t hr      = read_cmos_register(0x04);
    uint8_t dy      = read_cmos_register(0x07);
    uint8_t mo      = read_cmos_register(0x08);
    uint8_t yr      = read_cmos_register(0x09);
    uint8_t century = read_cmos_register(CMOS_REG_CENTURY);
    
    uint8_t regB    = read_cmos_register(0x0B);

    // Decode Binary Coded Decimal (BCD) format if active (Standard motherboard state)
    if (!(regB & 0x04)) {
        sec     = (sec & 0x0F)     + ((sec / 16) * 10);
        min     = (min & 0x0F)     + ((min / 16) * 10);
        hr      = (hr & 0x0F)      + (((hr & 0x70) / 16) * 10) | (hr & 0x80);
        dy      = (dy & 0x0F)     + ((dy / 16) * 10);
        mo      = (mo & 0x0F)     + ((mo / 16) * 10);
        yr      = (yr & 0x0F)     + ((yr / 16) * 10);
        century = (century & 0x0F) + ((century / 16) * 10);
    }

    // Convert 12-hour AM/PM format to clean 24-hour time if needed
    if (!(regB & 0x02) && (hr & 0x80)) {
        hr = ((hr & 0x7F) + 12) % 24;
    }

    // Calculate the full 4-digit century year. 
    // Fall back to year 2000 inference if the platform lacks Century Register 0x32.
    uint32_t full_year = (century == 0) ? (2000 + yr) : ((century * 100) + yr);

    // Populate the timespec structure
    ts->tv_sec  = calculate_epoch_seconds(full_year, mo, dy, hr, min, sec);
    ts->tv_nsec = 0; // CMOS cannot track sub-second nanoseconds
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;

    // Copy characters from src to dest up to the maximum limit 'n'
    // or until the end of the source string is reached
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }

    // If the source string was shorter than 'n', 
    // POSIX rules mandate padding the remaining bytes with null characters
    for (; i < n; i++) {
        dest[i] = '\0';
    }

    // Always return the original destination pointer address
    return dest;
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

static void clear_files(void) {
    for (size_t i = 0; i < file_count; i++) {
        // 1. DO NOT call kfree(files[i].path) anymore since it's a fixed array!
        // Instead, completely wipe out the path string character data block.
        memset(files[i].path, 0, 256);

        // 2. Safely free the data bytes payload if it was allocated via kmalloc
        if (files[i].data != NULL) {
            kfree(files[i].data);
            files[i].data = NULL;
        }

        // 3. Reset descriptor tracking attributes
        files[i].size = 0;
        files[i].mode = 0;
        files[i].uid = 0;
        files[i].gid = 0;
    }

    // Reset the active registration count index marker
    file_count = 0;
}

static int replace_file_data(struct vfs_file *file, const void *data, uint64_t size) {
    uint8_t *mirror = NULL;
    if (size != 0) {
        mirror = kmalloc((size_t)size);
        if (mirror == NULL) return -1;
        memcpy(mirror, data, (size_t)size);
    }
    kfree(file->data);
    file->data = mirror;
    file->size = size;
    return 0;
}

struct dentry *find_child(struct dentry *parent, const char *component, size_t len) {
    if (parent == NULL || component == NULL || len == 0) return NULL;

    for (size_t i = 0; i < parent->child_count; i++) {
        struct dentry *child = parent->children[i];
        if (__builtin_memcmp(child->name, component, len) == 0 && child->name[len] == '\0') {
            return child;
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

    static uint32_t global_ino_counter = 1;
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

static int append_file(const char *path, uint64_t path_size, const uint8_t *data, uint64_t size, uint32_t mode, uint32_t uid, uint32_t gid, uint64_t mtime) {
    if (path == NULL || path_size == 0) return -1;
    if (file_count >= MAX_VFS_FILES) return -1;

    // 1. Completely zero out the static path buffer inside the targeted slot first
    memset(files[file_count].path, 0, 256);

    // 2. Bound check path_size to ensure it safely fits within the 256-byte array buffer
    size_t copy_len = (size_t)path_size;
    if (copy_len >= 256) {
        copy_len = 255; // Leave the final room entry for the null terminator
    }

    // 3. Directly copy string characters from raw bootloader/archive memory into the static array
    memcpy(files[file_count].path, path, copy_len);
    files[file_count].path[copy_len] = '\0'; // Hard boundary safety enforcement

    // 4. ZERO KMALLOC BYPASS: Point directly to the persistent ramdisk memory address.
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
    
    // Root dentry initializes with default root permissions (0/0)
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
            printk(LOG_INFO, "--- RAW CPIO HEADER FOR %s ---\n", path);
            
            // Print the raw 6-character magic number (e.g., "070701")
            char magic_buf[7] = {0};
            memcpy(magic_buf, header->c_magic, 6);
            printk(LOG_INFO, "  c_magic:    %s\n", magic_buf);

            // Print the raw 8-character ASCII hexadecimal string for c_mode
            char mode_buf[9] = {0};
            memcpy(mode_buf, header->c_mode, 8);
            printk(LOG_INFO, "  c_mode hex: %s\n", mode_buf);
            
            // Print what your parse_hex8 function actually evaluated it to
            printk(LOG_INFO, "  Parsed mode (octal): 0%o\n", (uint32_t)mode);
            printk(LOG_INFO, "------------------------------------------\n");
        size_t pure_path_len = strlen(path);
        int appended_fd = append_file(path, pure_path_len + 1, archive + data_offset, file_size, mode, uid, gid, mtime);
        
        if (appended_fd < 0) {
            printk(LOG_WARNING, "append_file failed! File entry was rejected by global tracker.\n");
            for(;;);
        }
        
        struct dentry *current_dir = root_dentry;
        const char *ptr = path;
        while (*ptr != '\0') {
            while (*ptr == '/') {
                ptr++;
            }
            if (*ptr == '\0') {
                printk(LOG_TRACE, "Path tracking hit trailing string terminator component edge.\n");
                break;
            }

            const char *component_start = ptr;
            while (*ptr != '\0' && *ptr != '/') {
                ptr++;
            }
            size_t component_len = (size_t)(ptr - component_start);

            struct dentry *next_node = find_child(current_dir, component_start, component_len);

            if (next_node == NULL) {
                uint32_t node_mode = (*ptr == '/') ? (S_IFDIR | 0755) : (uint32_t)mode;
                struct timespec spec = {0};
                rtc_get_time(&spec);
                next_node = create_dentry_node(component_start, component_len, node_mode, uid, gid, nlink,spec.tv_sec,current_dir);
                if (next_node == NULL) {
                    printk(LOG_ERROR, "Static array directory child allocation limits hit.\n");
                    return -1;
                }
                if (*ptr == '\0' && (node_mode & S_IFMT) == S_IFREG) {
                    next_node->inode->size = file_size;
                    next_node->inode->mode = mode; // <-- FIX: Copies the packed x bit and r/w permissions over!
                    next_node->inode->uid  = uid;  // <-- FIX: Syncs owner UID
                    next_node->inode->gid  = gid;  // <-- FIX: Syncs group GID
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
    if (fd < 0 || (size_t)fd >= file_count || buf == NULL) return -1;
    if (perms(fd, 0)) {
        struct vfs_file *file = &files[fd];
        if (offset >= file->size) return 0;
        if (offset + count > file->size) {
            count = (size_t)(file->size - offset);
        }

        memcpy(buf, (const uint8_t *)file->data + offset, count);
        return (int)count; 
    } else return -EACCES;
}

int vfs_open(const char *path) {
    if (path == NULL) return -1;
    const char *lookup_path = path;
    if (lookup_path[0] == '/') {
        lookup_path++;
    } else if (lookup_path[0] == '.' && lookup_path[1] == '/') {
        lookup_path += 2;
    }

    for (size_t i = 0; i < file_count; i++) {
        const char *stored_path = files[i].path;
        if (stored_path == NULL) continue;

        if (stored_path[0] == '/') {
            stored_path++;
        } else if (stored_path[0] == '.' && stored_path[1] == '/') {
            stored_path += 2;
        }

        if (strcmp(stored_path, lookup_path) == 0) {
            return (int)i; 
        }
    }
    return -1; 
}

struct dentry *vfs_lookup_parent(const char *path) {
    if (path == NULL || path[0] != '/') return NULL;

    struct dentry *current = root_dentry;
    const char *ptr = path + 1;

    // We need to keep track of the last valid directory we successfully verified
    struct dentry *parent = NULL;

    while (*ptr != '\0') {
        while (*ptr == '/') ptr++;
        if (*ptr == '\0') break;

        const char *component_start = ptr;
        while (*ptr != '\0' && *ptr != '/') ptr++;
        size_t component_len = (size_t)(ptr - component_start);

        if (component_len > 0) {
            // The current node is the parent of the next component we are looking up
            parent = current; 
            
            current = find_child(current, component_start, component_len);
            if (current == NULL) {
                // If it's the very last component (the file itself), 'parent' is correct!
                if (*ptr == '\0') return parent;
                return NULL; 
            }
        }
    }
    return parent;
}
struct dentry *vfs_lookup_parent_by_flat_fd(int fd) {
    if (fd < 0 || (size_t)fd >= file_count) return NULL;

    // Use the absolute path stored in your flat array slot
    const char *path = files[fd].path; 
    
    // Safety check: Your path might not have a leading '/' depending on how it was parsed
    // We walk down the tree starting at root_dentry
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
            // Track the last valid directory visited before stepping deeper
            parent = current; 
            
            current = find_child(current, component_start, component_len);
            if (current == NULL) {
                // If the next child doesn't exist but it was the final filename component, 
                // 'parent' correctly holds the containing directory dentry node.
                if (*ptr == '\0') return parent;
                return NULL; 
            }
        }
    }
    return parent;
}
int vfs_write_file(int fd, const void *data, uint64_t size) {
    int bypass = perms(fd, 1);
    if (bypass) {
        if (fd < 0 || (size_t)fd >= file_count || (size != 0 && data == NULL)) return -1;
        return replace_file_data(&files[fd], data, size);
    }
}

int vfs_move_file(int fd, const char *newpath) {
    if (fd < 0 || (size_t)fd >= file_count || newpath == NULL) {
        return -1;
    }

    struct dentry *old_parent = vfs_lookup_parent_by_flat_fd(fd);
    struct dentry *new_parent = vfs_lookup_parent(newpath);

    // Verify Write (1) and Execute (2) permissions on both parent directories directly
    if (!permdir(old_parent, 1) || !permdir(old_parent, 2) ||
        !permdir(new_parent, 1) || !permdir(new_parent, 2)) {
        return -EACCES;
    }

    // Permissions passed; safely update the flat array path string
    memset(files[fd].path, 0, 256);
    strncpy(files[fd].path, newpath, 255);
    files[fd].path[255] = '\0';

    return 0;
}
int vfs_create_file(const void *data, const char *path, int dlen) {
    // Newly created files defaults to root context ownership (0/0)
    struct timespec ts;
    rtc_get_time(&ts);
    if (append_file(path, strlen(path) + 1, data, dlen, 0x8000, 0, 0, ts.tv_sec) < 0) return -1;
    return (int)(file_count - 1); 
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

int vfs_free_fd(int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || global_fd_table[fd] == NULL) return -1;
    kfree(global_fd_table[fd]);
    global_fd_table[fd] = NULL;
    return 0;
}

int vfs_delete_file(const char *path) {
    struct dentry *target = vfs_lookup(path);
    if (target == NULL || target->parent == NULL) return -1;

    struct dentry *parent = target->parent;

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (global_fd_table[i] != NULL && global_fd_table[i]->dentry == target) {
            kfree(global_fd_table[i]);
            global_fd_table[i] = NULL; 
        }
    }

    for (size_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == target) {
            for (size_t j = i; j < parent->child_count - 1; j++) {
                parent->children[j] = parent->children[j + 1];
            }
            parent->child_count--;
            break;
        }
    }

    if (target->inode) kfree(target->inode);
    kfree(target->name);
    kfree(target);
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

        // Enforce Execute permission to traverse through the current directory component
        if (!permdir(current_dir, 2)) {
            return -EACCES;
        }

        struct dentry *next_node = find_child(current_dir, component_start, component_len);

        if (next_node == NULL) {
            if (*ptr == '\0') {
                if (current_dir->inode && (current_dir->inode->mode & S_IFMT) != S_IFDIR) {
                    return -1;
                }
                
                // Enforce Write permission on the parent directory where the new node will live
                if (!permdir(current_dir, 1)) {
                    return -EACCES;
                }

                struct timespec ts;
                rtc_get_time(&ts);
                
                // Hardcodes UID/GID to 0 for system runtime actions
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
    if (target->child_count > 0) return -1; // Directory must be empty

    struct dentry *parent = target->parent;

    // To remove a directory description entry, you need Write (1) and Execute (2) on the parent folder
    if (!permdir(parent, 1) || !permdir(parent, 2)) {
        return -EACCES;
    }

    // Unlink the target from the parent's child list
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
    if (path == NULL || st == NULL)
        return -1;

    struct dentry *d = vfs_lookup(path);
    if (d == NULL || d->inode == NULL)
        return -1;

    st->st_ino   = d->inode->ino_num;
    st->st_mode  = d->inode->mode;
    st->st_uid   = d->inode->uid;   // Map UID
    st->st_gid   = d->inode->gid;   // Map GID
    st->st_size  = d->inode->size;
    st->st_nlink = d->inode->links;
    st->st_ctim = d->inode->st_ctim;
    st->st_mtim = d->inode->st_mtim;
    st->st_atim = d->inode->st_atim;

    return 0;
}
int vfs_fstat(int fd, struct vfs_stat *st) {
    if (st == NULL)
        return -2;

    if (fd < 0 || fd >= MAX_OPEN_FILES)
        return -3;

    struct file *file = global_fd_table[fd];
    
    // --- FALLBACK TO FLAT ARRAYS INDEX LINKAGE IF RUNTIME ELEMENT IS NULL ---
    if (file == NULL) {
        // If this index points to a valid file inside your ramdisk files database
        if ((size_t)fd < vfs_file_count()) {
            struct vfs_file *vf = &files[fd];
            
            st->st_ino     = (uint32_t)(fd + 100); // Generate a unique virtual inode
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
            return 0; // Success!
        }
        return -4; // Neither tracking structure has an entry
    }

    // Standard structural path fallback if global_fd_table entry IS present
    struct inode *inode = file->dentry->inode;
    if (inode == NULL)
        return -5;

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
    // 1. Structural validation boundaries
    if (buf == NULL || count < sizeof(struct vfs_dirent)) {
        return -EINVAL;
    }
    if (fd < 0 || fd >= MAX_OPEN_FILES) {
        return -EBADF;
    }
    if (perms(fd, 0)) {
        struct file *file = global_fd_table[fd];
        if (file == NULL || file->dentry == NULL || file->dentry->inode == NULL) {
            return -EBADF;
        }

        struct dentry *dir_dentry = file->dentry;
        
        // 2. Verify that this file descriptor actually points to an active directory node
        if ((dir_dentry->inode->mode & S_IFMT) != S_IFDIR) {
            return -ENOTDIR;
        }

        uint8_t *user_buf = (uint8_t *)buf;
        size_t bytes_written = 0;
        
        // 'offset' functions as the logical index tracking which child index we are on
        uint64_t child_idx = offset;

        // 3. Step through the fixed array of child dentries
        while (child_idx < dir_dentry->child_count) {
            struct dentry *child = dir_dentry->children[child_idx];
            if (child == NULL || child->inode == NULL || child->name == NULL) {
                child_idx++;
                continue;
            }

            // Check if there is enough space remaining in the user buffer for a structural instance
            if (bytes_written + sizeof(struct vfs_dirent) > count) {
                // If we haven't written any data at all, the total buffer capacity given is too small
                if (bytes_written == 0) return -EINVAL;
                break;
            }

            // 4. Map and populate the target vfs_dirent memory layout block
            struct vfs_dirent *vd = (struct vfs_dirent *)(user_buf + bytes_written);
            vd->d_ino = child->inode->ino_num;
            
            // Parse the internal inode type into matching POSIX DT_DIR or DT_REG codes
            if ((child->inode->mode & S_IFMT) == S_IFDIR) {
                vd->d_type = 4; // DT_DIR
            } else {
                vd->d_type = 8; // DT_REG
            }

            // Safely extract and bound the filename string into the 256-byte static destination buffer
            memset(vd->d_name, 0, 256);
            strncpy(vd->d_name, child->name, 255);
            vd->d_name[255] = '\0';

            bytes_written += sizeof(struct vfs_dirent);
            child_idx++;
        }

        // Returns total bytes populated inside the buffer stream, or 0 if EOF is reached
        return (int)bytes_written;
    } else {
        return -EACCES;
    }
}
#include <drivers/alloc.h> // Ensure kmalloc/kfree are accessible here

// We use the 'struct file' type alias internally to act as the POSIX DIR stream context
typedef struct file DIR;

/**
 * opens a directory stream corresponding to the directory named by the path.
 * Returns a pointer to the directory stream, or NULL on failure.
 */
DIR *opendir(const char *path) {
    if (path == NULL) return NULL;

    // Look up the dentry node associated with the requested path
    struct dentry *dir_dentry = vfs_lookup(path);
    if (dir_dentry == NULL || dir_dentry->inode == NULL) {
        return NULL; // Directory not found
    }

    // Verify that the targeted node is actually a directory
    if ((dir_dentry->inode->mode & S_IFMT) != S_IFDIR) {
        return NULL; // Not a directory
    }

    // Enforce Read permission tracking on the directory itself
    if (!permdir(dir_dentry, 0)) {
        return NULL; // Access Denied
    }

    // Allocate a runtime descriptor tracking block for the directory stream
    DIR *dir_stream = kmalloc(sizeof(DIR));
    if (dir_stream == NULL) {
        return NULL; // Out of memory
    }

    dir_stream->dentry = dir_dentry;
    dir_stream->offset = 0; // Initialize our child index tracker to 0
    dir_stream->flags  = 0;

    return dir_stream;
}

/**
 * Returns a pointer to a vfs_dirent structure representing the next directory entry 
 * in the directory stream pointed to by dirp. Returns NULL on EOF or failure.
 */
struct vfs_dirent *readdir(DIR *dirp) {
    // Validate stream and target node traits
    if (dirp == NULL || dirp->dentry == NULL || dirp->dentry->inode == NULL) {
        return NULL;
    }

    struct dentry *dir_dentry = dirp->dentry;

    // Re-verify permissions before reading the stream entries
    if (!permdir(dir_dentry, 0)) {
        return NULL;
    }

    // Maintain a static allocation structure to safely pass back memory references
    static struct vfs_dirent entry;

    // Step through the static child array using the cursor tracking 'offset'
    while (dirp->offset < dir_dentry->child_count) {
        struct dentry *child = dir_dentry->children[dirp->offset];
        dirp->offset++; // Advance cursor automatically for the next call

        if (child == NULL || child->inode == NULL || child->name == NULL) {
            continue; // Skip any unallocated slots or corrupt elements
        }

        // Map internal metadata details over to standard POSIX structure format
        entry.d_ino = child->inode->ino_num;
        
        if ((child->inode->mode & S_IFMT) == S_IFDIR) {
            entry.d_type = 4; // DT_DIR
        } else {
            entry.d_type = 8; // DT_REG
        }

        // Safely extract filename into static buffer zone
        memset(entry.d_name, 0, 256);
        strncpy(entry.d_name, child->name, 255);
        entry.d_name[255] = '\0';

        return &entry; // Return pointer to populated structural data context
    }

    return NULL; // Reached End of Directory (EOF)
}

/**
 * Closes the directory stream associated with dirp.
 * Returns 0 on success, or -1 on error.
 */
int closedir(DIR *dirp) {
    if (dirp == NULL) {
        return -1;
    }
    
    // Deallocate the execution state tracking block
    kfree(dirp);
    return 0;
}