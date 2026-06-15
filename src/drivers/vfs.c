// SPDX-License-Identifier: GPL-3.0-only
#include <drivers/alloc.h>
#include <drivers/fb.h>
#include <drivers/vfs.h>
#include <limine.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
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
#define MAX_OPEN_FILES 32
#define MAX_VFS_FILES  128  // Set a safe maximum for your ramdisk files

// The array that securely holds the raw VFS file pointers
static struct file *global_fd_table[MAX_OPEN_FILES];

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

static struct vfs_file files[MAX_VFS_FILES]; // Static array (no krealloc needed!)
static size_t file_count = 0;

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

static struct dentry *create_dentry_node(const char *name, size_t name_len, uint32_t mode, struct dentry *parent) {
    if (parent != NULL && parent->child_count >= MAX_DIR_CHILDREN) {
        printk("VFS ERROR: Maximum directory child limit (%d) reached!\n", MAX_DIR_CHILDREN);
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
    in->size = 0;
    in->data = NULL;

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
static int append_file(const char *path, uint64_t path_size, const uint8_t *data, uint64_t size, uint32_t mode) {
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
    // We cast away the 'const' qualifier so it fits into your existing uint8_t* struct field.
    files[file_count].data = (uint8_t *)data;
    files[file_count].size = size;
    files[file_count].mode = mode;

    int created_fd = (int)file_count;
    file_count++;
    
    return created_fd; 
}
int init_vfs(void) {
    printk("[VFS_DEBUG] --- ENTERING init_vfs ---\n");

    struct limine_file *initramfs_file = find_initramfs_module(module_request.response);
    if (initramfs_file == NULL) {
        printk("[VFS_DEBUG] CRITICAL: initramfs module pointer is NULL from Limine.\n");
        return -1;
    }

    printk("[VFS_DEBUG] initramfs module found at physical address: 0x%llx, size: %d bytes\n", 
           (unsigned long long)initramfs_file->address, (unsigned long long)initramfs_file->size);

    printk("[VFS_DEBUG] Allocating virtual file system root dentry...\n");
    root_dentry = create_dentry_node("", 0, S_IFDIR | 0755, NULL);
    if (root_dentry == NULL) {
        printk("[VFS_DEBUG] CRITICAL: Failed to allocate root dentry node memory.\n");
        return -1;
    }
    printk("[VFS_DEBUG] Root dentry initialized successfully at memory address: %p\n", (void*)root_dentry);

    const uint8_t *archive = (const uint8_t *)initramfs_file->address;
    uint64_t archive_size = initramfs_file->size;
    uint64_t offset = 0;
    size_t loaded_count = 0;
    size_t record_index = 0;

    printk("[VFS_DEBUG] Starting CPIO Archive Parsing Loop...\n");
    while (offset + sizeof(struct cpio_newc_header) <= archive_size) {
        const struct cpio_newc_header *header = (const struct cpio_newc_header *)(archive + offset);
        
        // Print raw magic bytes safely
        char magic_buf[7] = {0};
        memcpy(magic_buf, header->c_magic, 6);
        printk("[VFS_DEBUG]   Raw CPIO Header Magic String: \"%s\"\n", magic_buf);

        if (memcmp(header->c_magic, "070701", 6) != 0) {
            printk("[VFS_DEBUG]   CRITICAL ERROR: Invalid CPIO format signature magic mismatch.\n");
            return -1;
        }

        uint64_t mode = 0, file_size = 0, name_size = 0;
        parse_hex8(header->c_mode, &mode);
        parse_hex8(header->c_filesize, &file_size);
        parse_hex8(header->c_namesize, &name_size);

        uint64_t name_offset = offset + sizeof(struct cpio_newc_header);
        uint64_t data_offset = align4(name_offset + name_size);
        uint64_t next_offset = align4(data_offset + file_size);

        // Safety check to avoid reading out of bounds if headers are corrupted
        if (name_offset + name_size > archive_size || data_offset + file_size > archive_size) {
            printk("[VFS_DEBUG]   CRITICAL ERROR: Header metrics exceed boundaries of loaded ramdisk image.\n");
            return -1;
        }

        const char *path = (const char *)(archive + name_offset);
        if (strcmp(path, ".") == 0) {
            printk("[VFS_DEBUG]   Skipping explicit current-directory reference token \".\"\n");
            offset = next_offset;
            continue;
        }
        if (strcmp(path, "TRAILER!!!") == 0) {
            printk("[VFS_DEBUG]   SUCCESS: Found CPIO End-of-Archive End marker \"TRAILER!!!\"\n");
            printk("[VFS_DEBUG]   VFS Initialization summary: loaded %d hierarchical directory nodes.\n", loaded_count);
            printk("[VFS_DEBUG]   Global file tracking state: file_count is currently %d\n", vfs_file_count());
            printk("[VFS_DEBUG] --- EXITING init_vfs SUCCESSFULLY ---\n");
            return 0;
        }
        // Pass the pure string length to avoid saving hidden raw null terminators inside the path space
        size_t pure_path_len = strlen(path);
        int appended_fd = append_file(path, pure_path_len + 1, archive + data_offset, file_size, mode);
        
        if (appended_fd < 0) {
            printk("[VFS_DEBUG]     WARNING: append_file failed! File entry was rejected by global tracker.\n");
            for(;;);
        }
        struct dentry *current_dir = root_dentry;
        const char *ptr = path;
        while (*ptr != '\0') {
            // Strip multiple redundant path separators
            while (*ptr == '/') {
                ptr++;
            }
            if (*ptr == '\0') {
                printk("[VFS_DEBUG]     Path tracking hit trailing string terminator component edge.\n");
                break;
            }

            const char *component_start = ptr;
            while (*ptr != '\0' && *ptr != '/') {
                ptr++;
            }
            size_t component_len = (size_t)(ptr - component_start);

            // Output component slice details using printk with bounded size
            printk("[VFS_DEBUG]     Current Parent Dentry Context: \"%s\" (Children count: %d)\n", 
                   (current_dir->name && strlen(current_dir->name) > 0) ? current_dir->name : "/", current_dir->child_count);
            
            printk("[VFS_DEBUG]     Looking up sub-component: \"%.*s\" (Length: %d)\n", 
                   (int)component_len, component_start, (int)component_len);

            struct dentry *next_node = find_child(current_dir, component_start, component_len);

            if (next_node == NULL) {
                uint32_t node_mode = (*ptr == '/') ? (S_IFDIR | 0755) : (uint32_t)mode;
                printk("[VFS_DEBUG]       Component not found. Spawning missing node (Type: %s)...\n", 
                       (*ptr == '/') ? "DIRECTORY" : "REGULAR_FILE");
                
                next_node = create_dentry_node(component_start, component_len, node_mode, current_dir);
                if (next_node == NULL) {
                    printk("[VFS_DEBUG]       CRITICAL STRUCTURAL FAILURE: Static array directory child allocation limits hit.\n");
                    return -1;
                }
                printk("[VFS_DEBUG]       Node created successfully at address: %p assigned to Parent: %p\n", (void*)next_node, (void*)current_dir);
                
                if (*ptr == '\0' && (node_mode & S_IFMT) == S_IFREG) {
                    printk("[VFS_DEBUG]       Leaf reached: Binding persistent payload onto data inode descriptor\n");
                    next_node->inode->size = file_size;
                    
                    // FIXED: Read directly from the persistent array storage pointer instead of the raw archive offset
                    next_node->inode->data = files[appended_fd].data; 
                    
                    printk("[VFS_DEBUG]       Inode Target Size mapped: %d bytes, Address offset: %p\n", 
                           (unsigned long long)next_node->inode->size, (const void*)next_node->inode->data);
                }
                loaded_count++;
            } else {
                printk("[VFS_DEBUG]       Component found matching sub-node address: %p (Name: \"%s\")\n", (void*)next_node, next_node->name);
            }
            
            current_dir = next_node;
        }
        
        offset = next_offset;
    }

    printk("[VFS_DEBUG] CRITICAL: Loop finished naturally without reaching a TRAILER!!! marker sequence.\n");
    return -1;
}

size_t vfs_file_count(void) {
    return file_count;
}

int vfs_read(int fd, void *buf, size_t count, uint64_t offset) {
    if (fd < 0 || (size_t)fd >= file_count || buf == NULL) return -1;

    struct vfs_file *file = &files[fd];
    if (offset >= file->size) return 0;
    if (offset + count > file->size) {
        count = (size_t)(file->size - offset);
    }

    memcpy(buf, (const uint8_t *)file->data + offset, count);
    return (int)count; 
}

// FIX: Normalized search logic so leading slashes and paths like "./" match flawlessly
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

        printk("[OPEN_DEBUG] Normalized Target Match: '%s' vs Array Field: '%s'\n", lookup_path, stored_path);

        if (strcmp(stored_path, lookup_path) == 0) {
            return (int)i; 
        }
    }
    return -1; 
}

int vfs_write_file(int fd, const void *data, uint64_t size) {
    if (fd < 0 || (size_t)fd >= file_count || (size != 0 && data == NULL)) return -1;
    return replace_file_data(&files[fd], data, size);
}

int vfs_move_file(int fd, const char *newpath) {
    // Safety check: Ensure the file descriptor is valid and newpath exists
    if (fd < 0 || (size_t)fd >= file_count || newpath == NULL) {
        return -1;
    }

    // 1. Completely zero out the existing path string buffer
    memset(files[fd].path, 0, 256);

    // 2. Safely copy the string characters up to the maximum 255 byte boundary limit
    // Leave the 256th byte as a hard forced null-terminator ('\0')
    strncpy(files[fd].path, newpath, 255);
    files[fd].path[255] = '\0';

    return 0; // File path successfully modified inside the static table
}
int vfs_create_file(const void *data, const char *path, int dlen) {
    if (append_file(path, strlen(path) + 1, data, dlen, 0x8000) < 0) return -1;
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

        struct dentry *next_node = find_child(current_dir, component_start, component_len);

        if (next_node == NULL) {
            if (*ptr == '\0') {
                if (current_dir->inode && (current_dir->inode->mode & S_IFMT) != S_IFDIR) {
                    return -1;
                }
                next_node = create_dentry_node(component_start, component_len, S_IFDIR | (mode & 0777), current_dir);
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