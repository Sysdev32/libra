#include <fs/mnt.h>
#include <string.h>
#include <drivers/fb.h>
#include <drivers/alloc.h>
#include <fs/chfs.h>
#include <hals/ahci.h> // Required for proper struct sizing
#define BIT(x) (1ULL << (x))

// Replicating the CHFS internal caching structure perfectly to match chfs.c memory boundaries
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
    ahci_device_t dev;  // MUST be by value to match chfs.c layout
    uint64_t start_lba;
} chfs_cached_t;

// Accessing the global metadata tracking array instantiated inside chfs.c
extern chfs_cached_t metadata[32];
extern uint32_t global_ino_counter;

// Prototypes for foreign file system operations called by routing branches
extern int read_chfs(int disk, char* path, void* buffer, int offset, int count);
extern int write_chfs(int disk, char* path, void* buffer, size_t size);
extern int create_chfs(int disk, char* path);

mountpoint_t mountpoints[32] = {0};
full_t fd_table[32];
devfs_file files[64];
int last_fd = 0;
int8_t get_lowest_mnt() {
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

int8_t mount(partition_t *partition, char *path) {
    printk(LOG_TRACE, "[MNT] mount: Invoked for path '%s'\n", path ? path : "NULL");
    if (!path || !partition) {
        printk(LOG_TRACE, "[MNT] mount: Failure - Null path or partition pointer passed\n");
        return -1;
    }
    
    mountpoint_t mnt;
    printk(LOG_TRACE, "[MNT] mount: Initialized local mountpoint container!\n");
    
    mnt.part = *partition;
    strcpy(mnt.path, path);
    
    int mntpoint = get_lowest_mnt();
    if (mntpoint < 0) {
        printk(LOG_TRACE, "[MNT] mount: Failure - Fetching lowest mountpoint failed with status %d\n", mntpoint);
        return mntpoint;
    }
    
    mountpoints[mntpoint] = mnt;
    mountpoints[mntpoint].allocated = 1;
    
    printk(LOG_TRACE, "[MNT] mount: Successfully assigned partition type %d to slot %d at path '%s'\n", (int)partition->type, mntpoint, path);
    return mntpoint;
}

void umount(int8_t mnt) {
    printk(LOG_TRACE, "[MNT] umount: Invoked for index %d\n", (int)mnt);
    if (mnt >= 0 && mnt < 32) {
        mountpoints[mnt].allocated = 0;
        printk(LOG_TRACE, "[MNT] umount: Deallocated slot index %d successfully\n", (int)mnt);
    } else {
        printk(LOG_TRACE, "[MNT] umount: Out-of-bounds slot index %d provided\n", (int)mnt);
    }
}

size_t path_split(const char* src, char* dest_buf, char** out_tokens) {
    printk(LOG_TRACE, "[MNT] path_split: Splitting string input\n");
    size_t token_count = 0;
    size_t src_idx = 0;
    size_t dest_idx = 0;

    while (src[src_idx] != '\0') {
        while (src[src_idx] == '/') {
            src_idx++;
        }
        if (src[src_idx] == '\0') {
            break;
        }
        out_tokens[token_count] = &dest_buf[dest_idx];
        token_count++;

        while (src[src_idx] != '\0' && src[src_idx] != '/') {
            dest_buf[dest_idx] = src[src_idx];
            dest_idx++;
            src_idx++;
        }
        dest_buf[dest_idx] = '\0';
        dest_idx++;
    }
    out_tokens[token_count] = NULL; 
    
    printk(LOG_TRACE, "[MNT] path_split: Processed total tokens counted: %d\n", (int)token_count);
    return token_count;
}

void string_shift(char* str, int shift_index) {
    printk(LOG_TRACE, "[MNT] string_shift: Requested displacement index %d\n", shift_index);
    if (!str || shift_index <= 0) {
        printk(LOG_TRACE, "[MNT] string_shift: Aborted - Null buffer or index zero or less\n");
        return;
    }

    size_t len = strlen(str);
    if ((size_t)shift_index >= len) {
        printk(LOG_TRACE, "[MNT] string_shift: Shift index >= length (%d >= %d). Wiping string buffer\n", shift_index, (int)len);
        memset(str, 0, len);
        return;
    }

    size_t move_len = len - shift_index;
    memmove(str, str + shift_index, move_len);
    memset(str + move_len, 0, shift_index);
    printk(LOG_TRACE, "[MNT] string_shift: Operation complete. Resulting string path is '%s'\n", str);
}

int open(char* path, int flags, uint32_t mode) {
    printk(LOG_TRACE, "[MNT] open: Request initiated for path target '%s'\n", path ? path : "NULL");
    if (!path || path[0] == '\0') {
        printk(LOG_TRACE, "[MNT] open: Rejected - Path reference empty or null pointer\n");
        return -1;
    }

    char* tokens[32] = {0}; 
    char buf[256] = {0};
    size_t total_tokens = path_split(path, buf, tokens);
    
    int chars = 0;
    char current_prefix[256] = {0};

    for (size_t i = 0; i < total_tokens; i++) {
        strcat(current_prefix, "/");
        strcat(current_prefix, tokens[i]);
        chars += 1 + (int)strlen(tokens[i]); 
        
        printk(LOG_TRACE, "[MNT] open: Matching lookup index %d, testing target prefix boundary '%s'\n", (int)i, current_prefix);

        int partition = -1;
        for (int l = 0; l < 32; l++) {
            if (mountpoints[l].allocated && strcmp(mountpoints[l].path, current_prefix) == 0) {
                partition = l;
                break;
            }
        }

        if (partition != -1) {
            printk(LOG_TRACE, "[MNT] open: Bound match located at partition table slot index %d\n", partition);
            char mutable_path[256] = {0};
            strcpy(mutable_path, path);
            string_shift(mutable_path, chars);

            if (mutable_path[0] != '/') {
                char temp[256];
                temp[0] = '/';
                temp[1] = '\0';
                strcat(temp, mutable_path);
                strcpy(mutable_path, temp);
            }
            printk(LOG_TRACE, "[MNT] open: Formatted interior active subsystem directory route path: '%s'\n", mutable_path);

            // --- ROUTING BRANCH: VFS ---
            if (mountpoints[partition].part.type == VFS) {
                printk(LOG_TRACE, "[MNT] open: Executing routing branch -> Virtual File System (VFS)\n");
                return vfs_open(mutable_path, flags, mode);
            } 
            
            // --- ROUTING BRANCH: FAT32 ---
            else if (mountpoints[partition].part.type == FAT32) {
                printk(LOG_TRACE, "[MNT] open: Executing routing branch -> FAT32 File System\n");
                char* new_tokens[32] = {0};
                char bf[256] = {0};
                size_t local_tokens = path_split(mutable_path, bf, new_tokens);

                if (local_tokens == 0) {
                    printk(LOG_TRACE, "[MNT] open [FAT32]: Failure - Resolved path depth evaluates to zero tokens\n");
                    return -1; 
                }

                fat32_fs_t* fat = &mountpoints[partition].part.fat;
                uint32_t current_cluster = fat->root_cluster;
                uint32_t parent_cluster = fat->root_cluster; 
                fat32_entry_t final_entry = {0};
                bool lookup_success = true;

                for (size_t j = 0; j < local_tokens; j++) {
                    parent_cluster = current_cluster; 
                    printk(LOG_TRACE, "[MNT] open [FAT32]: Looking up node component object token '%s'\n", new_tokens[j]);
                    uint32_t next = fat32_find_object_lfn(fat, current_cluster, new_tokens[j], &final_entry);
                    
                    if (next == 0 && j < local_tokens - 1) {
                        printk(LOG_TRACE, "[MNT] open [FAT32]: Intermediate tree element navigation node missing\n");
                        lookup_success = false;
                        break;
                    }
                    if (j < local_tokens - 1) {
                        current_cluster = next; 
                    }
                }

                if (!lookup_success) {
                    printk(LOG_TRACE, "[MNT] open [FAT32]: Path resolution lookup failed\n");
                    return -1;
                }

                uint32_t file_size = final_entry.file_size;
                void* buffer = NULL;
                printk(LOG_TRACE, "[MNT] open [FAT32]: File target resolution tracking metric size: %d bytes\n", (int)file_size);
                
                if (file_size > 0) {
                    buffer = kmalloc(file_size);
                    if (!buffer) {
                        printk(LOG_TRACE, "[MNT] open [FAT32]: Failure allocating required sector reading cache buffer memory\n");
                        return -1;
                    }

                    uint32_t read_bytes = fat32_read_file_lfn(fat, parent_cluster, new_tokens[local_tokens - 1], buffer, file_size);
                    if (read_bytes == 0) {
                        printk(LOG_TRACE, "[MNT] open [FAT32]: Zero bytes retrieved from driver stream read evaluation\n");
                        kfree(buffer);
                        return -1;
                    }
                }

                if (last_fd >= 32) {
                    printk(LOG_TRACE, "[MNT] open [FAT32]: Global file description allocation boundaries exceeded limit: %d\n", last_fd);
                    if (buffer) kfree(buffer);
                    return -1; 
                }

                struct vfs_file in;
                in.data = buffer;
                in.gid = 0;
                in.uid = 0;
                in.nlink = 1;
                in.mode = 0100755; 
                in.size = file_size;
                strcpy(in.path, mutable_path);

                uint32_t target_file_cluster = ((uint32_t)final_entry.first_cluster_high << 16) | final_entry.first_cluster_low;

                int assigned_fd = last_fd;
                fd_table[last_fd].ord = in;
                fd_table[last_fd].file_cluster = target_file_cluster;
                fd_table[last_fd].dir_cluster = parent_cluster;       
                fd_table[last_fd].mountpoint = mountpoints[partition];
                strcpy(fd_table[last_fd].resolved, mutable_path);    
                last_fd++;
                
                printk(LOG_TRACE, "[MNT] open [FAT32]: Complete mapping sequence! Handing out descriptor: %d\n", assigned_fd);
                return assigned_fd;
            }
            
            // --- ROUTING BRANCH: CHFS ---
            else if (mountpoints[partition].part.type == CHFS) {
                printk(LOG_TRACE, "[MNT] open: Executing routing branch -> CHFS File System Layer\n");
                int disk_idx = mountpoints[partition].part.chfs;
                chfs_cached_t* cache = &metadata[disk_idx];
                CHFS_FHDR* fhdr_table = (CHFS_FHDR*)cache->fhdr;
                
                int fhdr_match_idx = -1;
                printk(LOG_TRACE, "[MNT] open [CHFS]: Parsing header entry logs array total size boundary %d files\n", (int)cache->file_count);
                for (int file_idx = 0; file_idx < cache->file_count; file_idx++) {
                    if (strcmp(fhdr_table[file_idx].path, mutable_path) == 0) {
                        fhdr_match_idx = file_idx;
                        break;
                    }
                }
                
                if (fhdr_match_idx == -1) {
                    printk(LOG_TRACE, "[MNT] open [CHFS]: Path match target validation failure for '%s'\n", mutable_path);
                    return -1; 
                }

                uint64_t file_size = fhdr_table[fhdr_match_idx].size;
                void* buffer = NULL;
                printk(LOG_TRACE, "[MNT] open [CHFS]: Target size configuration reported is %d bytes\n", (int)file_size);

                if (file_size > 0) {
                    buffer = kmalloc(file_size);
                    if (!buffer) {
                        printk(LOG_TRACE, "[MNT] open [CHFS]: Buffer memory frame creation exhausted resource pools\n");
                        return -1;
                    }

                    int bytes_read = read_chfs(disk_idx, mutable_path, buffer, 0, (int)file_size);
                    if (bytes_read < 0) {
                        printk(LOG_TRACE, "[MNT] open [CHFS]: Internal error return encountered during raw filesystem extraction: %d\n", bytes_read);
                        kfree(buffer);
                        return -1;
                    }
                }

                if (last_fd >= 32) {
                    printk(LOG_TRACE, "[MNT] open [CHFS]: Reached operating max threshold limit on standard process file tracking descriptors\n");
                    if (buffer) kfree(buffer);
                    return -1;
                }

                struct vfs_file in;
                in.data = buffer;
                in.gid = 0;
                in.uid = 0;
                in.nlink = 1;
                in.mode = 0100644; 
                in.size = file_size;
                strcpy(in.path, mutable_path);

                int assigned_fd = last_fd;
                fd_table[last_fd].ord = in;
                fd_table[last_fd].file_cluster = fhdr_match_idx; 
                fd_table[last_fd].dir_cluster = 0;
                fd_table[last_fd].mountpoint = mountpoints[partition];
                strcpy(fd_table[last_fd].resolved, mutable_path);
                last_fd++;
                
                printk(LOG_TRACE, "[MNT] open [CHFS]: Initialized file track setup loop. Assigning descriptor slot allocation index: %d\n", assigned_fd);
                return assigned_fd;
            } else if (mountpoints[partition].part.type == DEVFS) {
                int fi = -1;
                for (int i=0; i<64; i++) {
                    if (files[i].allocated && !strcmp(files[i].name, mutable_path)) {
                        fi = i;
                        break;
                    }
                }
                if (fi == -1) return -1;
                if (last_fd >= 32) return -1;
                int assigned_fd = last_fd;
                fd_table[assigned_fd].file = files[fi];
                if (fd_table[assigned_fd].file.type == DISK) {
                    fd_table[assigned_fd].file.tty = assigned_fd;
                }
                fd_table[assigned_fd].mountpoint = mountpoints[partition];
                strcpy(fd_table[assigned_fd].resolved, mutable_path);
                last_fd++;
                return assigned_fd;
            }
        }
    }
    printk(LOG_TRACE, "[MNT] open: Terminating - Zero matching volume mounts discovered across parameters context\n");
    return -1; 
}

size_t buffer_crop_to_output(const void* src, size_t src_total_sz, 
                             void* dest, size_t offset, size_t count) {
    printk(LOG_TRACE, "[MNT] buffer_crop_to_output: Extraction execution requested from size %d, offset %d, chunk total count %d\n", (int)src_total_sz, (int)offset, (int)count);
    if (!src || !dest || count == 0) {
        printk(LOG_TRACE, "[MNT] buffer_crop_to_output: Safe rejection - Pointer verification fault or absolute zero length requested\n");
        return 0;
    }

    const char* src_bytes = (const char*)src;
    char* dest_bytes = (char*)dest;

    if (offset >= src_total_sz) {
        printk(LOG_TRACE, "[MNT] buffer_crop_to_output: Shift configuration request offset parameter breaks maximum boundary thresholds\n");
        return 0;
    }
    if (offset + count > src_total_sz) {
        count = src_total_sz - offset;
        printk(LOG_TRACE, "[MNT] buffer_crop_to_output: Adjusted read operational configuration window payload length to %d\n", (int)count);
    }

    for (size_t i = 0; i < count; i++) {
        dest_bytes[i] = src_bytes[offset + i];
    }
    return count;
}

int read(int fd, void *buf, size_t count, uint64_t offset) {
    printk(LOG_TRACE, "[MNT] read: Action query evaluated for descriptor slot %d, payload count requests %d, offset positioning %d\n", fd, (int)count, (int)offset);
    if (fd < 0 || fd >= last_fd) {
        printk(LOG_TRACE, "[MNT] read: Out-of-bounds or unallocated descriptor key request submitted\n");
        return -1;
    }
    if (fd_table[fd].mountpoint.part.type != DEVFS) {
        return (int)buffer_crop_to_output(fd_table[fd].ord.data, fd_table[fd].ord.size, buf, (size_t)offset, count);
    } else {
        if (fd_table[fd].file.bitmask & DEVFS_READ) {
            return (int)fd_table[fd].file.read(fd_table[fd].file.tty, buf, count, (int)offset);
        } else {
            return -1;
        }
    }
}

const char* path_basename(const char* path) {
    printk(LOG_TRACE, "[MNT] path_basename: Extracting terminal filename string token element sequence out from original source path '%s'\n", path ? path : "NULL");
    if (!path || path[0] == '\0') return "";

    size_t len = strlen(path);
    int idx = (int)len - 1;

    while (idx >= 0 && path[idx] == '/') {
        idx--;
    }
    if (idx < 0) return "/";

    while (idx >= 0 && path[idx] != '/') {
        idx--;
    }
    
    const char* base = &path[idx + 1];
    printk(LOG_TRACE, "[MNT] path_basename: Extracted file leaf component results evaluate to: '%s'\n", base);
    return base;
}

int write(int fd, const void *data, uint64_t size) {
    printk(LOG_TRACE, "[MNT] write: Processing stream changes update for descriptor slot ID target: %d, stream payload length: %d\n", fd, (int)size);
    if (fd < 0 || fd >= last_fd) {
        printk(LOG_TRACE, "[MNT] write: Target evaluation rejected - Descriptor slot registry allocation key bounds breach\n");
        return -1;
    }

    if (fd_table[fd].mountpoint.part.type == VFS) {
        printk(LOG_TRACE, "[MNT] write [VFS]: Initiating block sector modifications across target pipeline path: '%s'\n", fd_table[fd].resolved);
        int fdi = vfs_open(fd_table[fd].resolved, O_WRONLY, 0);
        vfs_write_file(fdi, data, (size_t)size);
    } 
    else if (fd_table[fd].mountpoint.part.type == FAT32) {
        printk(LOG_TRACE, "[MNT] write [FAT32]: Initializing data injection to volume structures allocation segments\n");
        fat32_write_file_lfn(&fd_table[fd].mountpoint.part.fat, fd_table[fd].dir_cluster, path_basename(fd_table[fd].ord.path), data, (uint32_t)size);
    } 
    else if (fd_table[fd].mountpoint.part.type == CHFS) {
        int chfs_disk = fd_table[fd].mountpoint.part.chfs;
        printk(LOG_TRACE, "[MNT] write [CHFS]: Initializing structural modifications across hardware disk target volume ID %d\n", chfs_disk);
        int status = write_chfs(chfs_disk, fd_table[fd].resolved, (void*)data, (size_t)size);
        if (status < 0) {
            printk(LOG_TRACE, "[MNT] write [CHFS]: Internal driver engine failure reported via status flag metrics: %d\n", status);
            return -1;
        }
    } else if (fd_table[fd].mountpoint.part.type == DEVFS) {
        if (fd_table[fd].file.bitmask & DEVFS_WRITE) {
            return (int)fd_table[fd].file.write(fd_table[fd].file.tty, data, (size_t)size);
        }
        return -1;
    }
    if (fd_table[fd].mountpoint.part.type != DEVFS) {
        printk(LOG_TRACE, "[MNT] write: Re-caching dynamic data contents tracking inside runtime mirror tables structures\n");
        if (fd_table[fd].ord.data) {
            kfree(fd_table[fd].ord.data);
        }
        
        fd_table[fd].ord.data = kmalloc((size_t)size);
        if (fd_table[fd].ord.data) {
            memcpy(fd_table[fd].ord.data, data, (size_t)size);
        } else {
            printk(LOG_TRACE, "[MNT] write: Cache reallocation failure occurred during mirror framework update processing\n");
        }
        fd_table[fd].ord.size = (size_t)size;

        printk(LOG_TRACE, "[MNT] write: Updates deployed completely\n");
        return 0;
    }
    return -1;
}

int create(char* path) {
    printk(LOG_TRACE, "[MNT] create: Node creation process initialized for asset destination reference allocation mapping route path '%s'\n", path ? path : "NULL");
    if (!path || path[0] == '\0') {
        printk(LOG_TRACE, "[MNT] create: System parsing configuration cancelled - Destination parameter path is invalid or empty\n");
        return -1;
    }

    char* tokens[32] = {0}; 
    char buf[256] = {0};
    size_t total_tokens = path_split(path, buf, tokens);
    
    int chars = 0;
    char current_prefix[256] = {0};

    for (size_t i = 0; i < total_tokens; i++) {
        strcat(current_prefix, "/");
        strcat(current_prefix, tokens[i]);
        chars += 1 + (int)strlen(tokens[i]); 
        
        printk(LOG_TRACE, "[MNT] create: Testing parent prefix boundaries matching condition for value string: '%s'\n", current_prefix);

        int partition = -1;
        for (int l = 0; l < 32; l++) {
            if (mountpoints[l].allocated && strcmp(mountpoints[l].path, current_prefix) == 0) {
                partition = l;
                break;
            }
        }

        if (partition != -1) {
            printk(LOG_TRACE, "[MNT] create: Found matching partition assignment cluster reference profile index slot ID: %d\n", partition);
            char mutable_path[256] = {0};
            strcpy(mutable_path, path);
            string_shift(mutable_path, chars);

            if (mutable_path[0] != '/') {
                char temp[256];
                temp[0] = '/';
                temp[1] = '\0';
                strcat(temp, mutable_path);
                strcpy(mutable_path, temp);
            }
            printk(LOG_TRACE, "[MNT] create: Generated sub-system translation route path argument profile reference target: '%s'\n", mutable_path);

            if (mountpoints[partition].part.type == VFS) {
                printk(LOG_TRACE, "[MNT] create [VFS]: Processing initialization parameters sequence towards VFS subsystem driver infrastructure\n");
                char data[1] = "";
                vfs_create_file(data, mutable_path, 1);
                return 0;
            } 
            else if (mountpoints[partition].part.type == FAT32) {
                printk(LOG_TRACE, "[MNT] create [FAT32]: Processing node entry allocation sequences inside cluster maps indices tables\n");
                char* new_tokens[32] = {0};
                char bf[256] = {0};
                size_t local_tokens = path_split(mutable_path, bf, new_tokens);

                if (local_tokens == 0) {
                    printk(LOG_TRACE, "[MNT] create [FAT32]: Operational node directory hierarchy validation step fault\n");
                    return -1; 
                }

                fat32_fs_t* fat = &mountpoints[partition].part.fat;
                uint32_t current_cluster = fat->root_cluster;
                uint32_t parent_cluster = fat->root_cluster; 
                fat32_entry_t final_entry = {0};
                bool lookup_success = true;

                for (size_t j = 0; j < local_tokens; j++) {
                    parent_cluster = current_cluster; 
                    printk(LOG_TRACE, "[MNT] create [FAT32]: Tracking segment tree node directory boundary lookup: '%s'\n", new_tokens[j]);
                    uint32_t next = fat32_find_object_lfn(fat, current_cluster, new_tokens[j], &final_entry);
                    
                    if (next == 0 && j < local_tokens - 1) {
                        printk(LOG_TRACE, "[MNT] create [FAT32]: Intermediate tree reference folder hierarchy tracking failed\n");
                        lookup_success = false;
                        break;
                    }
                    if (j < local_tokens - 1) {
                        current_cluster = next; 
                    }
                }

                if (!lookup_success) {
                    printk(LOG_TRACE, "[MNT] create [FAT32]: Execution failed due to missing components along the path route map hierarchy\n");
                    return -1;
                }
                
                printk(LOG_TRACE, "[MNT] create [FAT32]: Calling low-level filesystem implementation wrapper block to write entity record named: '%s'\n", final_entry.name);
                return fat32_create_file_lfn(fat, parent_cluster, final_entry.name, 0);
            }
            else if (mountpoints[partition].part.type == CHFS) {
                int chfs_disk = mountpoints[partition].part.chfs;
                printk(LOG_TRACE, "[MNT] create [CHFS]: Initializing block map expansion steps against hardware storage target unit tracking profile ID %d\n", chfs_disk);
                return create_chfs(chfs_disk, mutable_path);
            }
        }
    }
    printk(LOG_TRACE, "[MNT] create: Resolution routing phase aborted - Zero matching mounts mappings matches valid signatures inside registry arrays tables\n");
    return -1; 
}
void register_device(read_func_t read, ioctl_func_t ioctl, write_func_t write,
                     uint8_t bitmask, DevFsType type, char* name,
                     ahci_device_t dev, int tty) {
    printk(LOG_TRACE, "[MNT] register_device: Registering device '%s'\n", name ? name : "NULL");

    if (!name) {
        return;
    }

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
                                    name[3] == 'v' && name[4] == '/') ? name + 4 : name;
            strncpy(files[i].name, dev_name, sizeof(files[i].name) - 1);
            files[i].name[sizeof(files[i].name) - 1] = '\0';
            files[i].allocated = true;
            return;
        }
    }

    printk(LOG_TRACE, "[MNT] register_device: No free device slots available for '%s'\n", name);
}
