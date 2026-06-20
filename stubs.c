#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include <errno.h>
#ifndef SEEK_SET
#define SEEK_SET 0
#endif

#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif

#ifndef SEEK_END
#define SEEK_END 2
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#undef errno
extern int errno;

// --- User-Space Bump Allocator Configuration ---
// These symbols can be exported from your user space linker script (.ld)
// Example: _heap_start at the end of .bss, and _heap_end matching your app's memory limit.
extern char _heap_start; 
extern char _heap_end;

// Manages low-level heap growth completely in Ring 3
caddr_t sbrk(int incr) {
    static char *heap_end = NULL;
    char *prev_heap_end;

    if (heap_end == NULL) {
        heap_end = &_heap_start;
    }

    // Verify we have enough room in our pre-allocated user memory segment
    if (heap_end + incr > &_heap_end) {
        errno = ENOMEM;
        return (caddr_t)-1;
    }

    prev_heap_end = heap_end;
    heap_end += incr; // Bump the allocation pointer forward

    return (caddr_t)prev_heap_end; // Return the start of the allocated segment
}

typedef struct {
    uint64_t arg[8];
} syscall_args_t;

// --- System Call Routing to Kernel (No Case 9 needed) ---
static inline long user_syscall3(long num, long a1, long a2, long a3) {
    syscall_args_t args = {
        .arg = {
            (uint64_t)a1,
            (uint64_t)a2,
            (uint64_t)a3,
        },
    };
    long ret;
    __asm__ volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (num), "b" (&args)
        : "cc", "memory"
    );
    return ret;
}
static inline long user_syscall4(long num, long a1, long a2, long a3, long a4) {
    syscall_args_t args = {
        .arg = {
            (uint64_t)a1,
            (uint64_t)a2,
            (uint64_t)a3,
            (uint64_t)a4
        },
    };
    long ret;
    __asm__ volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (num), "b" (&args)
        : "cc", "memory"
    );
    return ret;
}
static inline long user_syscall7(long num, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
    syscall_args_t args = {
        .arg = {
            (uint64_t)a1,
            (uint64_t)a2,
            (uint64_t)a3,
            (uint64_t)a4,
            (uint64_t)a5,
            (uint64_t)a6,
            (uint64_t)a7,
        },
    };
    long ret;
    __asm__ volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (num), "b" (&args)
        : "cc", "memory"
    );
    return ret;
}
// Define a maximum threshold for concurrent descriptors tracked by your app
#define MAX_TRACKED_FDS 64
#define SYS_READ  0
#define SYS_WRITE 1
#define SYS_OPEN  2
// Array to manage the tracking offset for each active kernel file descriptor
static uint64_t vfs_fd_offsets[MAX_TRACKED_FDS] = {0};

// --- Updated open stub ---
int open(const char *name, int flags, int mode) {
    // Triggers case 2 in your kernel dispatcher
    long ret = user_syscall3(SYS_OPEN, (long)name, (long)flags, (long)mode);
    if (ret < 0) { 
        errno = -ret; 
        return -1; 
    }
    
    int kernel_fd = (int)ret;
    
    // Ensure we do not overflow our local tracking buffer boundaries
    if (kernel_fd >= 0 && kernel_fd < MAX_TRACKED_FDS) {
        vfs_fd_offsets[kernel_fd] = 0; // Initialize the read/write tracker at 0
    }
    
    // Return the shifted Newlib descriptor (Kernel FD + 2)
    return kernel_fd + 2;
}

// --- Updated read stub ---
int read(int file, char *ptr, int len) {
    if (file <= 2) return 0; // Guard clause for standard streams
    
    int kernel_fd = file - 2;
    
    if (kernel_fd < 0 || kernel_fd >= MAX_TRACKED_FDS) {
        errno = EBADF;
        return -1;
    }
    
    // Retrieve the stateful tracking position for this descriptor
    uint64_t current_offset = vfs_fd_offsets[kernel_fd];
    
    // Call the updated 4-argument system router
    // This maps correctly to vfs_read(kernel_fd, ptr, len, current_offset)
    long ret = user_syscall4(SYS_READ, kernel_fd, (long)ptr, (long)len, current_offset);
    if (ret < 0) { 
        errno = -ret; 
        return -1; 
    }
    
    // CRITICAL: Advance the tracking position by the precise number of bytes read
    vfs_fd_offsets[kernel_fd] += ret;
    
    return (int)ret;
}

// --- Updated lseek stub ---
int lseek(int file, int offset, int dir) {
    if (file <= 2) return 0;
    
    int kernel_fd = file - 2;
    if (kernel_fd < 0 || kernel_fd >= MAX_TRACKED_FDS) {
        errno = EBADF;
        return -1;
    }
    
    if (dir == SEEK_SET) {
        vfs_fd_offsets[kernel_fd] = offset;
    } 
    else if (dir == SEEK_CUR) {
        vfs_fd_offsets[kernel_fd] += offset;
    } 
    else if (dir == SEEK_END) {
        // Since your VFS signature layer does not expose a "vfs_file_size" function,
        // SEEK_END cannot be calculated purely in user-space without kernel data.
        errno = ENOSYS;
        return -1;
    }
    
    return (int)vfs_fd_offsets[kernel_fd];
}

// --- Updated close stub ---
int close(int file) {
    if (file <= 2) return 0;
    
    int kernel_fd = file - 2;
    if (kernel_fd >= 0 && kernel_fd < MAX_TRACKED_FDS) {
        vfs_fd_offsets[kernel_fd] = 0; // Clear the memory index window
    }
    
    long ret = user_syscall3(5, kernel_fd, 0, 0); // case 5: vfs_free_fd
    if (ret < 0) { errno = -ret; return -1; }
    return 0;
}

int write(int file, char *ptr, int len) {
    long target_fd = (file > 2) ? (file - 2) : file;
    long ret = user_syscall3(SYS_WRITE, target_fd, (long)ptr, (long)len);
    if (ret < 0) { errno = -ret; return -1; }
    return (int)ret;
}
int fstat(int file, struct stat *st) { st->st_mode = S_IFCHR; return 0; }
int isatty(int file) { return 1; }
int getpid(void) { return 1; }
int kill(int pid, int sig) { errno = EINVAL; return -1; }
void _exit(int status) {
    user_syscall3(12, status, 0, 0);
    for(;;);
}
void graduate() {
    user_syscall3(10, 0, 0, 0);
}
void draw_rect(int rect_x, int rect_y, int rect_width, int rect_height, 
                   uint8_t r, uint8_t g, uint8_t b) {
    user_syscall7(11, rect_x, rect_y, rect_width, rect_height, b, g, r);
}
int ipc_send(uint32_t target_pid, const void *buf, uint32_t size) {
    user_syscall3(14, target_pid, (long)buf, (long)size);
}
int ipc_recv(void *buf, uint32_t max_size, uint32_t *out_sender_pid) {
    user_syscall3(13, (long)buf, max_size, (long)out_sender_pid);
}