#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

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

// --- System Call Routing to Kernel (No Case 9 needed) ---
static inline long user_syscall3(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a" (ret)
        : "a" (num), "D" (a1), "S" (a2), "d" (a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

#define SYS_READ  0
#define SYS_WRITE 1

int write(int file, char *ptr, int len) {
    long target_fd = (file > 2) ? (file - 2) : file;
    long ret = user_syscall3(SYS_WRITE, target_fd, (long)ptr, (long)len);
    if (ret < 0) { errno = -ret; return -1; }
    return (int)ret;
}

int read(int file, char *ptr, int len) {
    if (file <= 2) return 0; // Ignore or implement basic user stdin logic
    long ret = user_syscall3(SYS_READ, (file - 2), (long)ptr, (long)len);
    if (ret < 0) { errno = -ret; return -1; }
    return (int)ret;
}

int fstat(int file, struct stat *st) { st->st_mode = S_IFCHR; return 0; }
int isatty(int file) { return 1; }
int close(int file) { return -1; }
int lseek(int file, int ptr, int dir) { return 0; }
int getpid(void) { return 1; }
int kill(int pid, int sig) { errno = EINVAL; return -1; }
void _exit(int status) { while (1) { __asm__ volatile ("pause"); } }