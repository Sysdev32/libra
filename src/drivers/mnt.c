#include <drivers/mnt.h>
#include <string.h>
#include <drivers/fb.h>
extern uint32_t global_ino_counter;

mountpoint_t mountpoints[32] = {0};
full_t fd_table[32];
static int last_fd = 0;
static int last_mountpoint = 0;
int8_t get_lowest_mnt() {
    for (int i=0; i<32; i++) {
        if (!mountpoints[i].allocated) {
            return i;
        }
    }
    return -1;
}
int8_t mount(partition_t *partition, char *path) {
    if (!path) return -1;
    if (!partition) return -1;
    mountpoint_t mnt;
    printk(LOG_TRACE, "[MNT] Initialized mountpoint!\n");
    mnt.part = *partition;
    strcpy(mnt.path, path);
    int mntpoint = get_lowest_mnt();
    if (mntpoint < 0) return mntpoint;
    mountpoints[mntpoint] = mnt;
    mountpoints[mntpoint].allocated = 1;
    
    return mntpoint;
}

void umount(int8_t mnt) {
    mountpoints[mnt].allocated = 0;
}
size_t path_split(const char* src, char* dest_buf, char** out_tokens) {
    size_t token_count = 0;
    size_t src_idx = 0;
    size_t dest_idx = 0;

    while (src[src_idx] != '\0') {
        // Skip leading or consecutive slashes
        while (src[src_idx] == '/') {
            src_idx++;
        }

        // If we hit the end of the string after slashes, we are done
        if (src[src_idx] == '\0') {
            break;
        }

        // Mark the start of this token in our destination buffer
        out_tokens[token_count] = &dest_buf[dest_idx];
        token_count++;

        // Copy characters until the next slash or end of string
        while (src[src_idx] != '\0' && src[src_idx] != '/') {
            dest_buf[dest_idx] = src[src_idx];
            dest_idx++;
            src_idx++;
        }

        // Null-terminate the current token in the destination buffer
        dest_buf[dest_idx] = '\0';
        dest_idx++;
    }

    // Null-terminate the pointer array so you know where it ends
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

    // memmove is safe for overlapping memory regions
    memmove(str, str + shift_index, move_len);

    // Fill the remaining stale data at the back with zeros
    memset(str + move_len, 0, shift_index);
}
typedef struct {
    int cluster;
    fat32_entry_t ent;
} entry;
int open(char* path) {
    if (!path || path[0] == '\0') return -1;

    char* tokens[32] = {0}; 
    char buf[256] = {0};
    size_t total_tokens = path_split(path, buf, tokens);
    
    int chars = 0;
    char current_prefix[256] = {0};

    // 1. Find which mountpoint matches our path prefix
    for (size_t i = 0; i < total_tokens; i++) {
        strcat(current_prefix, "/");
        strcat(current_prefix, tokens[i]);
        
        chars += 1 + strlen(tokens[i]); 

        int partition = -1;
        for (int l = 0; l < 32; l++) {
            if (mountpoints[l].allocated && strcmp(mountpoints[l].path, current_prefix) == 0) {
                partition = l;
                break;
            }
        }

        // If a matching mountpoint was discovered
        if (partition != -1) {
            char mutable_path[256] = {0};
            strcpy(mutable_path, path);
            string_shift(mutable_path, chars);

            // Handle VFS partition routing
            if (mountpoints[partition].part.type == VFS) {
                return vfs_open(mutable_path);
            } 
            
            // Handle FAT32 partition routing
            else if (mountpoints[partition].part.type == FAT32) {
                char* new_tokens[32] = {0};
                char bf[256] = {0};
                size_t local_tokens = path_split(mutable_path, bf, new_tokens);

                if (local_tokens == 0) return -1; 

                fat32_fs_t* fat = &mountpoints[partition].part.fat;
                uint32_t current_cluster = fat->root_cluster;
                uint32_t parent_cluster = fat->root_cluster; // Track the parent directory cluster
                fat32_entry_t final_entry = {0};
                bool lookup_success = true;

                // Walk down the subdirectories using the correct sequence
                for (size_t j = 0; j < local_tokens; j++) {
                    parent_cluster = current_cluster; // The current cluster is the parent for the next lookup
                    uint32_t next = fat32_find_object_lfn(fat, current_cluster, new_tokens[j], &final_entry);
                    
                    // If we failed to find an intermediate path or file entry
                    if (next == 0 && j < local_tokens - 1) {
                        lookup_success = false;
                        break;
                    }
                    
                    if (j < local_tokens - 1) {
                        current_cluster = next; 
                    }
                }

                if (!lookup_success) return -1;

                // 2. Allocate the EXACT memory size using metadata info
                uint32_t file_size = final_entry.file_size;
                void* buffer = NULL;
                
                if (file_size > 0) {
                    buffer = kmalloc(file_size);
                    if (!buffer) return -1;

                    // Read using the accurate last tracked parent directory cluster context
                    uint32_t read_bytes = fat32_read_file_lfn(fat, parent_cluster, new_tokens[local_tokens - 1], buffer, file_size);
                    if (read_bytes == 0) {
                        kfree(buffer);
                        return -1;
                    }
                }

                // 3. Populate our VFS File Table Entry Structure Safely
                if (last_fd >= 32) {
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
                
                memset(&in.st_atim, 0, sizeof(struct timespec));
                memset(&in.st_ctim, 0, sizeof(struct timespec));
                memset(&in.st_mtim, 0, sizeof(struct timespec));
                
                strcpy(in.path, mutable_path);

                // Extract the starting cluster of the file from the final FAT32 entry
                uint32_t target_file_cluster = ((uint32_t)final_entry.first_cluster_high << 16) | final_entry.first_cluster_low;

                int assigned_fd = last_fd;
                fd_table[last_fd].ord = in;
                fd_table[last_fd].file_cluster = target_file_cluster; // Target file cluster
                fd_table[last_fd].dir_cluster = parent_cluster;       // Parent directory cluster
                fd_table[last_fd].mountpoint = mountpoints[partition];
                strcpy(fd_table[last_fd].resolved, mutable_path);    
                last_fd++;
                return assigned_fd;
            }
        }
    }
    return -1; 
}

#include <stddef.h> // For size_t and NULL

/**
 * Copies a cropped chunk from a source buffer into a destination buffer.
 * Assumes the destination buffer is pre-allocated to hold at least 'count' bytes.
 *
 * @param src          The original source buffer (unmodified).
 * @param src_total_sz The total size of the source data block.
 * @param dest         The destination buffer to write into.
 * @param offset       The starting byte position inside the source buffer.
 * @param count        The number of bytes to extract and copy.
 * @return             The actual number of bytes successfully copied.
 */
size_t buffer_crop_to_output(const void* src, size_t src_total_sz, 
                             void* dest, size_t offset, size_t count) {
    if (!src || !dest || count == 0) return 0;

    const char* src_bytes = (const char*)src;
    char* dest_bytes = (char*)dest;

    // Safety check: If the offset is out of bounds, nothing can be copied
    if (offset >= src_total_sz) {
        return 0;
    }

    // Clamp count if it tries to read past the end of the source buffer
    if (offset + count > src_total_sz) {
        count = src_total_sz - offset;
    }

    // Copy exactly the required bytes into the destination
    for (size_t i = 0; i < count; i++) {
        dest_bytes[i] = src_bytes[offset + i];
    }

    return count; // Returns how many bytes were actually written
}

int read(int fd, void *buf, size_t count, uint64_t offset) {
    return buffer_crop_to_output(fd_table[fd].ord.data, fd_table[fd].ord.size, buf, offset, count);
}
const char* path_basename(const char* path) {
    if (!path || path[0] == '\0') {
        return "";
    }

    size_t len = strlen(path);
    int idx = (int)len - 1;

    // Strip trailing slashes (e.g., "/a/b/c/" -> "/a/b/c")
    while (idx >= 0 && path[idx] == '/') {
        idx--;
    }

    // If the path was only slashes (e.g., "///"), return "/"
    if (idx < 0) {
        return "/";
    }

    // Find the next slash looking backwards
    while (idx >= 0 && path[idx] != '/') {
        idx--;
    }

    // Point to the character right after the last slash
    return &path[idx + 1];
}

int write(int fd, const void *data, uint64_t size) {
    if (fd_table[fd].mountpoint.part.type == VFS) {
        // TODO: find a way not to call vfs_open here lol
        int fdi = vfs_open(fd_table[fd].resolved);
        vfs_write_file(fdi, data, size);
    } else if (fd_table[fd].mountpoint.part.type == FAT32) {
        fat32_write_file_lfn(&fd_table[fd].mountpoint.part.fat, fd_table[fd].dir_cluster, path_basename(fd_table[fd].ord.path), data, size);
    }
    memcpy(fd_table[fd].ord.data, data, size);
    return 0;
}