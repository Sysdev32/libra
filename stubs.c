#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include <reent.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
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
#define O_RDONLY    0x0000

bool grad = false;
#define HEAP_START ((char*)0x40000000)
#define HEAP_END   ((char*)0x50000000)
caddr_t sbrk(int incr) {
    static char *heap_end = NULL;

    if (!heap_end)
        heap_end = HEAP_START;

    if (heap_end + incr > HEAP_END) {
        _REENT->_errno = ENOMEM;
        return (caddr_t)-1;
    }

    char *prev = heap_end;
    heap_end += incr;
    return (caddr_t)prev;
}

typedef struct {
    uint64_t arg[8];
} syscall_args_t;

// --- System Call Routing Mechanisms ---
static inline long user_syscall3(long num, long a1, long a2, long a3) {
    syscall_args_t args = {
        .arg = { (uint64_t)a1, (uint64_t)a2, (uint64_t)a3 }
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
        .arg = { (uint64_t)a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4 }
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
            (uint64_t)a1, (uint64_t)a2, (uint64_t)a3, (uint64_t)a4,
            (uint64_t)a5, (uint64_t)a6, (uint64_t)a7
        }
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

#define MAX_TRACKED_FDS 64
#define SYS_READ  0
#define SYS_WRITE 1
#define SYS_OPEN  2
#define SYS_CLOSE 5
#define SYS_FSTAT 6
#define SYS_CREATEF 7

static uint64_t vfs_fd_offsets[MAX_TRACKED_FDS] = {0};
struct kernel_stat {
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


// --- Fully Aligned Standard Namespaces ---

int open(const char *name, int flags, int mode) {
    long ret = user_syscall3(SYS_OPEN, (long)name, (long)flags, (long)mode);
    if (ret < 0) { 
        _REENT->_errno = -ret; 
        return -1; 
    }
    
    int kernel_fd = (int)ret;
    if (kernel_fd >= 0 && kernel_fd < MAX_TRACKED_FDS) {
        vfs_fd_offsets[kernel_fd] = 0;
    }
    
    return kernel_fd + 2;
}

int createf(void* data, const char *name, int len) {
    long ret = user_syscall3(SYS_CREATEF, (long)data, (long)name, len);
    if (ret < 0) { 
        _REENT->_errno = -ret; 
        return -1; 
    }
    
    int kernel_fd = (int)ret;
    if (kernel_fd >= 0 && kernel_fd < MAX_TRACKED_FDS) {
        vfs_fd_offsets[kernel_fd] = 0;
    }
    
    return kernel_fd + 2;
}

int read(int file, char *ptr, int len) {
    if (file == 0) {
        return user_syscall3(18, (long)ptr, len, 0);
    }
    int kernel_fd = file - 2;
    if (kernel_fd < 0 || kernel_fd >= MAX_TRACKED_FDS) {
        _REENT->_errno = EBADF;
        return -1;
    }
    
    uint64_t current_offset = vfs_fd_offsets[kernel_fd];
    long ret = user_syscall4(SYS_READ, kernel_fd, (long)ptr, (long)len, current_offset);
    if (ret < 0) { 
        _REENT->_errno = -ret; 
        return -1; 
    }
    
    vfs_fd_offsets[kernel_fd] += ret;
    return (int)ret;
}

int write(int file, char *ptr, int len) {
    long target_fd = (file > 2) ? (file - 2) : file;
    long ret = user_syscall3(SYS_WRITE, target_fd, (long)ptr, (long)len);
    if (ret < 0) { 
        _REENT->_errno = -ret; 
        return -1; 
    }
    return (int)ret;
}

int fstat(int file, struct stat *st) {
    memset(st, 0, sizeof(struct stat));
    if (file <= 2) {
        st->st_mode = 0010000; 
        st->st_nlink = 1;
        return 0;
    }

    int kernel_fd = file - 2;
    if (kernel_fd < 0 || kernel_fd >= MAX_TRACKED_FDS) {
        _REENT->_errno = EBADF;
        return -1;
    }
    
    struct kernel_stat kst;
    long ret = user_syscall3(17, kernel_fd, (long)&kst, 0);
    

    if (ret < 0) {
        _REENT->_errno = -ret;
        return -1;
    }

    // Inspect the actual raw values pulled back inside struct kernel_stat
    // Copying variables across to standard target
    st->st_ino  = kst.st_ino;
    st->st_mode = kst.st_mode;
    st->st_size = kst.st_size;
    st->st_nlink   = kst.st_nlink;
    st->st_blksize = kst.st_blksize;
    st->st_blocks  = kst.st_blocks;
    st->st_dev     = kst.st_dev;
    st->st_rdev    = kst.st_rdev;
    st->st_uid     = kst.st_uid;
    st->st_gid     = kst.st_gid;

#if defined(_AT_HOME) || defined(__CYGWIN__) || defined(_POSIX_C_SOURCE)
    st->st_atim.tv_sec  = kst.st_atim.tv_sec;
    st->st_atim.tv_nsec = kst.st_atim.tv_nsec;
    st->st_mtim.tv_sec  = kst.st_mtim.tv_sec;
    st->st_mtim.tv_nsec = kst.st_mtim.tv_nsec;
    st->st_ctim.tv_sec  = kst.st_ctim.tv_sec;
    st->st_ctim.tv_nsec = kst.st_ctim.tv_nsec;
#else
    st->st_atime        = kst.st_atim.tv_sec;
    st->st_mtime        = kst.st_mtim.tv_sec;
    st->st_ctime        = kst.st_ctim.tv_sec;
#endif

    // Read back final verification values inside Newlib's target struct
    return 0;
}

int lseek(int file, int offset, int dir) {
    if (file <= 2) return 0;
    
    int kernel_fd = file - 2;
    if (kernel_fd < 0 || kernel_fd >= MAX_TRACKED_FDS) {
        _REENT->_errno = EBADF;
        return -1;
    }
    
    if (dir == SEEK_SET) {
        vfs_fd_offsets[kernel_fd] = offset;
    } 
    else if (dir == SEEK_CUR) {
        vfs_fd_offsets[kernel_fd] += offset;
    } 
    else if (dir == SEEK_END) {
        struct stat st;
        if (fstat(file, &st) < 0) {
            return -1; 
        }
        vfs_fd_offsets[kernel_fd] = st.st_size + offset;
    }
    
    return (int)vfs_fd_offsets[kernel_fd];
}

int close(int file) {
    if (file <= 2) return 0;
    
    int kernel_fd = file - 2;
    if (kernel_fd >= 0 && kernel_fd < MAX_TRACKED_FDS) {
        vfs_fd_offsets[kernel_fd] = 0;
    }
    
    long ret = user_syscall3(SYS_CLOSE, kernel_fd, 0, 0);
    if (ret < 0) { 
        _REENT->_errno = -ret; 
        return -1; 
    }
    return 0;
}

// --- Auxiliary Task Management Stubs ---
int isatty(int file) {
    if (file == 1 || file == 2) {
        if (!grad) {
            return 1;  
        } else {
            return 0;
        }
    } else {
        return 0;
    }
}
int getpid(void) { return user_syscall3(15, 0, 0, 0); }
int kill(int pid, int sig) { user_syscall3(16, pid, 0, 0); return 0; }

void _exit(int status) {
    user_syscall3(12, status, 0, 0);
    for(;;);
}

// --- Graphical and Local IPC Routing Additions ---
void graduate(void) {
    user_syscall3(10, 0, 0, 0);
    grad = true;
}

void draw_rect(int rect_x, int rect_y, int rect_width, int rect_height, 
               uint8_t r, uint8_t g, uint8_t b) {
    user_syscall7(11, rect_x, rect_y, rect_width, rect_height, b, g, r);
}

int ipc_send(uint32_t target_pid, const void *buf, uint32_t size) {
    long ret = user_syscall3(14, target_pid, (long)buf, (long)size);
    if (ret < 0) { _REENT->_errno = -ret; return -1; }
    return (int)ret;
}

int ipc_recv(void *buf, uint32_t max_size, uint32_t *out_sender_pid) {
    long ret = user_syscall3(13, (long)buf, max_size, (long)out_sender_pid);
    if (ret < 0) { _REENT->_errno = -ret; return -1; }
    return (int)ret;
}
int stat(const char *file, struct stat *st) {
    // 1. Open the file to get a file descriptor
    int fd = open(file, O_RDONLY, 0);
    if (fd < 0) {
        // _open is responsible for setting errno (e.g., ENOENT)
        return -1; 
    }

    // 2. Query the file stats using the file descriptor
    int result = fstat(fd, st);

    // 3. Clean up and close the file descriptor
    close(fd);

    return result;
}
#include <stdarg.h>

int unlink(const char *path) {
    errno = ENOSYS;
    return -1;
}

int fcntl(int fd, int cmd, ...) {
    (void)fd;
    (void)cmd;
    return 0;
}
void draw_image(int start_x, int start_y, int img_w, int img_h, const uint8_t *rgb_data) {
    user_syscall7(21, start_x, start_y, img_w, img_h, (long)rgb_data, 0, 0);
}