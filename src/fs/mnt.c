#include <fs/mnt.h>
#include <fs/vfs.h>
#include <string.h>
#include <drivers/fb.h>
#include <drivers/alloc.h>
#include <fs/chfs.h>
#include <hals/ahci.h>
#include <errno.h>
#include <fcntl.h>

#define BIT(x) (1ULL << (x))

// CHFS caching layout mirror
typedef struct {
    CHFS_HDR* hdr;
    CHFS_FHDR* fhdr;
    uint32_t* in_indexing;
    CHFS_JRN* journal;
    uint32_t* jn_indexing;
    uint16_t file_count;
    uint32_t inode_count;
    uint64_t journal_count;
    char vol_name[36];
    uint32_t free_fhdr;
    uint32_t free_inodes;
    ahci_device_t dev;
    uint64_t start_lba;
} chfs_cached_t;

extern chfs_cached_t metadata[32];
extern uint32_t global_ino_counter;

extern int read_chfs(int disk, char* path, void* buffer, int offset, int count);
extern int write_chfs(int disk, char* path, void* buffer, size_t size);
extern int create_chfs(int disk, char* path);

mountpoint_t mountpoints[32] = {0};
full_t fd_table[32];
devfs_file files[64] = {0};
int last_fd = 0;

int8_t get_lowest_mnt(void) {
    printk(LOG_TRACE, "[MNT] get_lowest_mnt: Scanning mountpoint slots\n");
    for (int i = 0; i < 32; i++) {
        if (!mountpoints[i].allocated) {
            printk(LOG_TRACE, "[MNT] get_lowest_mnt: Found available slot index %d\n", i);
            return i;
        }
    }
    printk(LOG_TRACE, "[MNT] get_lowest_mnt: Error - No free mountpoints available\n");
    return -1;
}

int8_t mount(initialized_drive *drive, int vol_index, char *path) {
    printk(LOG_TRACE, "[MNT] mount: Invoked for path '%s'\n", path ? path : "NULL");
    if (!path || !drive) {
        printk(LOG_TRACE, "[MNT] mount: Failure - Null path or drive pointer passed\n");
        return -1;
    }

    // Check if path is already mounted
    for (int i = 0; i < 32; i++) {
        if (mountpoints[i].allocated && strcmp(mountpoints[i].path, path) == 0) {
            printk(LOG_TRACE, "[MNT] mount: Failure - Path '%s' already mounted\n", path);
            return -EBUSY;
        }
    }

    int mntpoint = get_lowest_mnt();
    if (mntpoint < 0) {
        printk(LOG_TRACE, "[MNT] mount: Failure - Fetching lowest mountpoint failed\n");
        return mntpoint;
    }

    mountpoints[mntpoint].drive = drive;
    mountpoints[mntpoint].vol_index = vol_index;
    strcpy(mountpoints[mntpoint].path, path);
    mountpoints[mntpoint].allocated = 1;
    mountpoints[mntpoint].is_devfs = false;

    printk(LOG_TRACE, "[MNT] mount: Successfully assigned drive to slot %d at path '%s'\n", mntpoint, path);
    return mntpoint;
}

void umount(int8_t mnt) {
    printk(LOG_TRACE, "[MNT] umount: Invoked for index %d\n", (int)mnt);
    if (mnt >= 0 && mnt < 32) {
        if (!mountpoints[mnt].allocated) {
            printk(LOG_TRACE, "[MNT] umount: Target slot index %d is not allocated\n", (int)mnt);
            return;
        }
        mountpoints[mnt].allocated = 0;
        mountpoints[mnt].drive = NULL;
        mountpoints[mnt].is_devfs = false;
        memset(mountpoints[mnt].path, 0, sizeof(mountpoints[mnt].path));
        printk(LOG_TRACE, "[MNT] umount: Deallocated slot index %d successfully\n", (int)mnt);
    } else {
        printk(LOG_TRACE, "[MNT] umount: Out-of-bounds slot index %d provided\n", (int)mnt);
    }
}

void devfs_init(void) {
    printk(LOG_TRACE, "[DEVFS] devfs_init: Initializing /dev pseudo-filesystem\n");
    memset(files, 0, sizeof(files));

    int mntpoint = get_lowest_mnt();
    if (mntpoint < 0) {
        printk(LOG_TRACE, "[DEVFS] devfs_init: Error - No free slot to mount /dev\n");
        return;
    }

    mountpoints[mntpoint].drive = NULL;
    mountpoints[mntpoint].vol_index = -1;
    strcpy(mountpoints[mntpoint].path, "/dev");
    mountpoints[mntpoint].allocated = 1;
    mountpoints[mntpoint].is_devfs = true;

    printk(LOG_TRACE, "[DEVFS] devfs_init: Successfully registered /dev at slot %d\n", mntpoint);
}

void register_device(read_func_t read, ioctl_func_t ioctl, write_func_t write,
                     uint8_t bitmask, DevFsType type, char* name,
                     ahci_device_t dev, int tty) {
    printk(LOG_TRACE, "[DEVFS] register_device: Registering device '%s'\n", name ? name : "NULL");

    if (!name) return;

    for (int i = 0; i < 64; i++) {
        if (!files[i].allocated) {
            files[i].read = read;
            files[i].ioctl = ioctl;
            files[i].write = write;
            files[i].bitmask = bitmask;
            files[i].type = type;
            files[i].dev = dev;
            files[i].tty = tty;

            const char *dev_name = (name[0] == '/' && name[1] == 'd' && name[2] == 'e' &&
                                    name[3] == 'v' && name[4] == '/') ? name + 5 : name;
            strncpy(files[i].name, dev_name, sizeof(files[i].name) - 1);
            files[i].name[sizeof(files[i].name) - 1] = '\0';
            files[i].allocated = true;
            return;
        }
    }

    printk(LOG_TRACE, "[DEVFS] register_device: No free device slots available for '%s'\n", name);
}

size_t path_split(const char* src, char* dest_buf, char** out_tokens) {
    size_t token_count = 0;
    size_t src_idx = 0;
    size_t dest_idx = 0;

    while (src[src_idx] != '\0') {
        while (src[src_idx] == '/') src_idx++;
        if (src[src_idx] == '\0') break;

        out_tokens[token_count++] = &dest_buf[dest_idx];

        while (src[src_idx] != '\0' && src[src_idx] != '/') {
            dest_buf[dest_idx++] = src[src_idx++];
        }
        dest_buf[dest_idx++] = '\0';
    }
    out_tokens[token_count] = NULL;
    return token_count;
}

void string_shift(char* str, int shift_index) {
    if (!str || shift_index <= 0) return;

    size_t len = strlen(str);
    if ((size_t)shift_index >= len) {
        memset(str, 0, len);
        return;
    }

    size_t move_len = len - shift_index;
    memmove(str, str + shift_index, move_len);
    memset(str + move_len, 0, shift_index);
}

static inline fat32_fs_t* get_fat32_instance(mountpoint_t* mnt) {
    if (!mnt || !mnt->drive) return NULL;
    int vi = mnt->vol_index;
    return &mnt->drive->format.gpt_partition_table.vols[vi].fs.filesystem;
}

// Helper: Resolve a path to its associated mountpoint and local target path
static int resolve_mount(const char* path, mountpoint_t** out_mnt, char* local_path_out) {
    if (!path || path[0] == '\0') return -EINVAL;

    char* tokens[32] = {0};
    char buf[256] = {0};
    size_t total_tokens = path_split(path, buf, tokens);

    int chars = 0;
    char current_prefix[256] = {0};
    int best_match = -1;
    int best_chars = 0;

    for (size_t i = 0; i < total_tokens; i++) {
        strcat(current_prefix, "/");
        strcat(current_prefix, tokens[i]);
        chars += 1 + (int)strlen(tokens[i]);

        for (int l = 0; l < 32; l++) {
            if (mountpoints[l].allocated && strcmp(mountpoints[l].path, current_prefix) == 0) {
                best_match = l;
                best_chars = chars;
                break;
            }
        }
    }

    if (best_match != -1) {
        *out_mnt = &mountpoints[best_match];
        strcpy(local_path_out, path);
        string_shift(local_path_out, best_chars);
        if (local_path_out[0] != '/') {
            char temp[256] = "/";
            strcat(temp, local_path_out);
            strcpy(local_path_out, temp);
        }
        return 0;
    }

    *out_mnt = NULL;
    strcpy(local_path_out, path);
    return 0;
}

int open(char* path, int flags, uint32_t mode) {
    printk(LOG_TRACE, "[MNT] open: Request initiated for path target '%s'\n", path ? path : "NULL");
    if (!path || path[0] == '\0') return -EINVAL;

    mountpoint_t* mnt = NULL;
    char local_path[256] = {0};
    resolve_mount(path, &mnt, local_path);

    if (mnt) {
        // --- DEVFS ROUTING ---
        if (mnt->is_devfs) {
            int fi = -1;
            const char *search_name = (local_path[0] == '/') ? local_path + 1 : local_path;

            for (int i = 0; i < 64; i++) {
                if (files[i].allocated && strcmp(files[i].name, search_name) == 0) {
                    fi = i;
                    break;
                }
            }
            // Fallback to VFS open if target device missing in devfs
            if (fi == -1) return vfs_open(path, flags, mode);
            if (last_fd >= 32) return -EMFILE;

            int assigned_fd = last_fd;
            fd_table[assigned_fd].file = files[fi];
            if (fd_table[assigned_fd].file.type == DISK) {
                fd_table[assigned_fd].file.tty = assigned_fd;
            }
            fd_table[assigned_fd].mountpoint = *mnt;
            strcpy(fd_table[assigned_fd].resolved, local_path);
            last_fd++;
            return assigned_fd;
        }

        // --- FAT32 ROUTING ---
        if (mnt->drive) {
            int vi = mnt->vol_index;
            FileSystem fs_type = mnt->drive->format.gpt_partition_table.vols[vi].fsType;

            if (fs_type == FS_FAT32) {
                printk(LOG_TRACE, "[MNT] open: Executing routing branch -> FAT32 File System\n");
                char* new_tokens[32] = {0};
                char bf[256] = {0};
                size_t local_tokens = path_split(local_path, bf, new_tokens);

                if (local_tokens == 0) return vfs_open(path, flags, mode);

                fat32_fs_t* fat = get_fat32_instance(mnt);
                uint32_t current_cluster = fat->root_cluster;
                uint32_t parent_cluster = fat->root_cluster;
                fat32_entry_t final_entry = {0};
                bool lookup_success = true;

                for (size_t j = 0; j < local_tokens; j++) {
                    parent_cluster = current_cluster;
                    uint32_t next = fat32_find_object_lfn(fat, current_cluster, new_tokens[j], &final_entry);

                    if (next == 0 && j < local_tokens - 1) {
                        lookup_success = false;
                        break;
                    }
                    if (j < local_tokens - 1) current_cluster = next;
                }

                if (!lookup_success) return vfs_open(path, flags, mode);

                uint32_t file_size = final_entry.file_size;
                void* buffer = NULL;

                if (file_size > 0) {
                    buffer = kmalloc(file_size);
                    if (!buffer) return -ENOMEM;

                    uint32_t read_bytes = fat32_read_file_lfn(fat, parent_cluster, new_tokens[local_tokens - 1], buffer, file_size);
                    if (read_bytes == 0) {
                        kfree(buffer);
                        return -EIO;
                    }
                }

                if (last_fd >= 32) {
                    if (buffer) kfree(buffer);
                    return -EMFILE;
                }

                struct vfs_file in;
                in.data = buffer;
                in.gid = 0;
                in.uid = 0;
                in.nlink = 1;
                in.mode = 0100755;
                in.size = file_size;
                strcpy(in.path, local_path);

                uint32_t target_file_cluster = ((uint32_t)final_entry.first_cluster_high << 16) | final_entry.first_cluster_low;

                int assigned_fd = last_fd;
                fd_table[last_fd].ord = in;
                fd_table[last_fd].file_cluster = target_file_cluster;
                fd_table[last_fd].dir_cluster = parent_cluster;
                fd_table[last_fd].mountpoint = *mnt;
                strcpy(fd_table[last_fd].resolved, local_path);
                last_fd++;

                return assigned_fd;
            }
        }
    }

    // --- VFS FALLBACK ROUTING ---
    return vfs_open(path, flags, mode);
}

size_t buffer_crop_to_output(const void* src, size_t src_total_sz, void* dest, size_t offset, size_t count) {
    if (!src || !dest || count == 0) return 0;
    if (offset >= src_total_sz) return 0;

    if (offset + count > src_total_sz) {
        count = src_total_sz - offset;
    }

    memcpy(dest, (const char*)src + offset, count);
    return count;
}

int read(int fd, void *buf, size_t count, uint64_t offset) {
    if (fd < 0) return -EBADF;

    if (fd < last_fd) {
        if (fd_table[fd].mountpoint.is_devfs) {
            if (fd_table[fd].file.bitmask & DEVFS_READ) {
                return (int)fd_table[fd].file.read(fd_table[fd].file.tty, buf, count, (int)offset);
            }
            return -EACCES;
        }

        if (fd_table[fd].mountpoint.allocated) {
            return (int)buffer_crop_to_output(fd_table[fd].ord.data, fd_table[fd].ord.size, buf, (size_t)offset, count);
        }
    }

    // Fall back to VFS descriptor/flat file read
    return vfs_read(fd, buf, count, offset);
}

const char* path_basename(const char* path) {
    if (!path || path[0] == '\0') return "";

    size_t len = strlen(path);
    int idx = (int)len - 1;

    while (idx >= 0 && path[idx] == '/') idx--;
    if (idx < 0) return "/";

    while (idx >= 0 && path[idx] != '/') idx--;

    return &path[idx + 1];
}

int write(int fd, const void *data, uint64_t size) {
    if (fd < 0) return -EBADF;

    if (fd < last_fd) {
        if (fd_table[fd].mountpoint.is_devfs) {
            if (fd_table[fd].file.bitmask & DEVFS_WRITE) {
                return (int)fd_table[fd].file.write(fd_table[fd].file.tty, data, (size_t)size);
            }
            return -EACCES;
        }

        if (fd_table[fd].mountpoint.allocated) {
            mountpoint_t* mnt = &fd_table[fd].mountpoint;
            fat32_fs_t* fat = get_fat32_instance(mnt);

            if (fat) {
                fat32_write_file_lfn(fat, fd_table[fd].dir_cluster, path_basename(fd_table[fd].ord.path), data, (uint32_t)size);
            }

            if (fd_table[fd].ord.data) kfree(fd_table[fd].ord.data);

            fd_table[fd].ord.data = kmalloc((size_t)size);
            if (fd_table[fd].ord.data) {
                memcpy(fd_table[fd].ord.data, data, (size_t)size);
            }
            fd_table[fd].ord.size = (size_t)size;

            return (int)size;
        }
    }

    // Fall back to VFS file write
    return vfs_write_file(fd, data, size);
}

int create(char* path) {
    if (!path || path[0] == '\0') return -EINVAL;

    mountpoint_t* mnt = NULL;
    char local_path[256] = {0};
    resolve_mount(path, &mnt, local_path);

    if (mnt) {
        if (mnt->is_devfs) return -EPERM;

        fat32_fs_t* fat = get_fat32_instance(mnt);
        if (fat) {
            char* new_tokens[32] = {0};
            char bf[256] = {0};
            size_t local_tokens = path_split(local_path, bf, new_tokens);

            if (local_tokens == 0) return vfs_create_file(NULL, path, 0);

            uint32_t current_cluster = fat->root_cluster;
            uint32_t parent_cluster = fat->root_cluster;
            fat32_entry_t final_entry = {0};

            for (size_t j = 0; j < local_tokens; j++) {
                parent_cluster = current_cluster;
                uint32_t next = fat32_find_object_lfn(fat, current_cluster, new_tokens[j], &final_entry);
                if (j < local_tokens - 1) {
                    if (next == 0) return vfs_create_file(NULL, path, 0);
                    current_cluster = next;
                }
            }

            return fat32_create_file_lfn(fat, parent_cluster, path_basename(local_path), 0);
        }
    }

    // Fall back to VFS file creation
    return vfs_create_file(NULL, path, 0);
}

// --- Extended VFS Wrappers with Mount Checks ---

int mnt_mkdir(const char *path, uint32_t mode) {
    if (!path) return -EINVAL;

    mountpoint_t* mnt = NULL;
    char local_path[256] = {0};
    resolve_mount(path, &mnt, local_path);

    if (mnt && mnt->is_devfs) {
        return -EPERM;
    }

    return vfs_mkdir(path, mode);
}

int mnt_rmdir(const char *path) {
    if (!path) return -EINVAL;

    mountpoint_t* mnt = NULL;
    char local_path[256] = {0};
    resolve_mount(path, &mnt, local_path);

    if (mnt && mnt->is_devfs) {
        return -EPERM;
    }

    return vfs_rmdir(path);
}

int mnt_unlink(const char *path) {
    if (!path) return -EINVAL;

    mountpoint_t* mnt = NULL;
    char local_path[256] = {0};
    resolve_mount(path, &mnt, local_path);

    if (mnt && mnt->is_devfs) {
        return -EPERM;
    }

    return vfs_delete_file(path);
}

int mnt_stat(const char *path, struct vfs_stat *st) {
    if (!path || !st) return -EINVAL;

    mountpoint_t* mnt = NULL;
    char local_path[256] = {0};
    resolve_mount(path, &mnt, local_path);

    if (mnt && mnt->is_devfs) {
        const char *search_name = (local_path[0] == '/') ? local_path + 1 : local_path;
        for (int i = 0; i < 64; i++) {
            if (files[i].allocated && strcmp(files[i].name, search_name) == 0) {
                memset(st, 0, sizeof(struct vfs_stat));
                st->st_ino = (uint32_t)(i + 500);
                st->st_mode = 0020666;
                st->st_nlink = 1;
                return 0;
            }
        }
        return -ENOENT;
    }

    return vfs_stat(path, st);
}

int mnt_fstat(int fd, struct vfs_stat *st) {
    if (!st) return -EINVAL;
    if (fd < 0) return -EBADF;

    if (fd < last_fd && fd_table[fd].mountpoint.allocated) {
        memset(st, 0, sizeof(struct vfs_stat));
        if (fd_table[fd].mountpoint.is_devfs) {
            st->st_ino = (uint32_t)(fd + 500);
            st->st_mode = 0020666;
            st->st_nlink = 1;
        } else {
            st->st_ino = fd_table[fd].file_cluster;
            st->st_mode = fd_table[fd].ord.mode;
            st->st_size = fd_table[fd].ord.size;
            st->st_nlink = fd_table[fd].ord.nlink;
        }
        return 0;
    }

    return vfs_fstat(fd, st);
}

int mnt_getdents(int fd, void *buf, size_t count, uint64_t offset) {
    if (!buf) return -EINVAL;
    if (fd < 0) return -EBADF;

    if (fd < last_fd && fd_table[fd].mountpoint.is_devfs) {
        if (count < sizeof(struct vfs_dirent)) return -EINVAL;

        uint8_t *user_buf = (uint8_t *)buf;
        size_t bytes_written = 0;
        uint64_t idx = offset;
        uint64_t active_count = 0;

        for (int i = 0; i < 64; i++) {
            if (!files[i].allocated) continue;

            if (active_count < idx) {
                active_count++;
                continue;
            }

            if (bytes_written + sizeof(struct vfs_dirent) > count) break;

            struct vfs_dirent *vd = (struct vfs_dirent *)(user_buf + bytes_written);
            vd->d_ino = (uint32_t)(i + 500);
            vd->d_type = 2;

            memset(vd->d_name, 0, 256);
            strncpy(vd->d_name, files[i].name, 255);

            bytes_written += sizeof(struct vfs_dirent);
            active_count++;
        }

        return (int)bytes_written;
    }

    return vfs_getdents(fd, buf, count, offset);
}

int close(int fd) {
    if (fd < 0) return -EBADF;

    if (fd < last_fd && fd_table[fd].mountpoint.allocated) {
        if (!fd_table[fd].mountpoint.is_devfs && fd_table[fd].ord.data) {
            kfree(fd_table[fd].ord.data);
            fd_table[fd].ord.data = NULL;
        }
        memset(&fd_table[fd], 0, sizeof(full_t));
        return 0;
    }

    return vfs_free_fd(fd);
}
int mnt_listdir(const char *path, char **out_names, size_t max_entries) {
    printk(LOG_TRACE, "[MNT_LISTDIR] Called for path: '%s', max_entries: %zu\n",
           path ? path : "NULL", max_entries);

    if (!path || !out_names || max_entries == 0) {
        printk(LOG_TRACE, "[MNT_LISTDIR] Error: Invalid argument(s) passed\n");
        return -EINVAL;
    }

    mountpoint_t *mnt = NULL;
    char local_path[256] = {0};
    resolve_mount(path, &mnt, local_path);

    printk(LOG_TRACE, "[MNT_LISTDIR] Resolved path -> mnt: %p, local_path: '%s'\n",
           (void*)mnt, local_path);

    if (mnt) {
        // --- 1. DEVFS ROUTING ---
        if (mnt->is_devfs) {
            printk(LOG_TRACE, "[MNT_LISTDIR] Target mountpoint is DEVFS\n");
            size_t count = 0;
            for (int i = 0; i < 64 && count < max_entries; i++) {
                if (!files[i].allocated) continue;
                if (!out_names[count]) return -EINVAL;

                strncpy(out_names[count], files[i].name, 255);
                out_names[count][255] = '\0';

                printk(LOG_TRACE, "[MNT_LISTDIR] DEVFS: Appended entry [%llu] -> '%s'\n", count, files[i].name);
                count++;
            }
            printk(LOG_TRACE, "[MNT_LISTDIR] DEVFS: Returning %llu entries\n", count);
            return (int)count;
        }

        // --- 2. FAT32 / DISK MOUNT ROUTING ---
        if (mnt->drive) {
            printk(LOG_TRACE, "[MNT_LISTDIR] Mountpoint has associated drive (%p), checking FAT32 instance...\n", (void*)mnt->drive);
            fat32_fs_t *fat = get_fat32_instance(mnt);
            if (fat) {
                printk(LOG_TRACE, "[MNT_LISTDIR] FAT32 filesystem handle located (root_cluster: %u)\n", fat->root_cluster);
                char *new_tokens[32] = {0};
                char bf[256] = {0};
                size_t local_tokens = path_split(local_path, bf, new_tokens);

                printk(LOG_TRACE, "[MNT_LISTDIR] Path split into %llu token(s)\n", local_tokens);

                uint32_t current_cluster = fat->root_cluster;
                fat32_entry_t final_entry = {0};
                bool lookup_failed = false;

                for (size_t j = 0; j < local_tokens; j++) {
                    printk(LOG_TRACE, "[MNT_LISTDIR] Traversing path token [%llu]: '%s' from cluster %u\n",
                           j, new_tokens[j], current_cluster);
                    uint32_t next = fat32_find_object_lfn(fat, current_cluster, new_tokens[j], &final_entry);
                    if (next == 0) {
                        printk(LOG_TRACE, "[MNT_LISTDIR] FAT32: Object '%s' not found under cluster %u\n",
                               new_tokens[j], current_cluster);
                        lookup_failed = true;
                        break;
                    }
                    current_cluster = next;
                    printk(LOG_TRACE, "[MNT_LISTDIR] Found object '%s', moved to next cluster %u\n",
                           new_tokens[j], current_cluster);
                }

                if (!lookup_failed) {
                    size_t dirent_buf_sz = max_entries * sizeof(struct vfs_dirent);
                    printk(LOG_TRACE, "[MNT_LISTDIR] Allocating dirent buffer (%llu bytes) for reading cluster %u\n",
                           dirent_buf_sz, current_cluster);

                    struct vfs_dirent *dir_buf = kmalloc(dirent_buf_sz);
                    if (!dir_buf) {
                        printk(LOG_TRACE, "[MNT_LISTDIR] Error: Failed to allocate dirent buffer\n");
                        return -ENOMEM;
                    }

                    int bytes_read = fat32_read_dir_lfn(fat, current_cluster, dir_buf, max_entries);
                    printk(LOG_TRACE, "[MNT_LISTDIR] fat32_read_dir_lfn returned %d bytes\n", bytes_read);

                    if (bytes_read >= 0) {
                        int entry_count = bytes_read / sizeof(struct vfs_dirent);
                        if (entry_count > (int)max_entries) entry_count = (int)max_entries;

                        printk(LOG_TRACE, "[MNT_LISTDIR] Processing %d dirent entry/entries\n", entry_count);

                        for (int i = 0; i < entry_count; i++) {
                            if (!out_names[i]) {
                                printk(LOG_TRACE, "[MNT_LISTDIR] Error: NULL buffer pointer at out_names[%d]\n", i);
                                kfree(dir_buf);
                                return -EINVAL;
                            }

                            strncpy(out_names[i], dir_buf[i].d_name, 255);
                            out_names[i][255] = '\0';

                            printk(LOG_TRACE, "[MNT_LISTDIR] FAT32: Appended entry [%d] -> '%s'\n", i, dir_buf[i].d_name);
                        }

                        kfree(dir_buf);
                        printk(LOG_TRACE, "[MNT_LISTDIR] Successfully returning %d entries\n", entry_count);
                        return entry_count;
                    }
                    kfree(dir_buf);
                }
            } else {
                printk(LOG_TRACE, "[MNT_LISTDIR] Failed to retrieve FAT32 instance from mountpoint\n");
            }
        }
    }

    // --- 3. VFS FALLBACK ROUTING ---
    printk(LOG_TRACE, "[MNT_LISTDIR] Falling back to vfs_listdir for path: '%s'\n", path);
    int vfs_res = vfs_listdir(path, out_names, max_entries);
    printk(LOG_TRACE, "[MNT_LISTDIR] vfs_listdir returned code: %d\n", vfs_res);

    return vfs_res;
}