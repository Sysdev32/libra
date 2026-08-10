#include <systable.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#define ENAMETOOLONG 36
#include <arch/x86_64/idt.h>
#include <arch/x86_64/schedule.h>
#include <drivers/fb.h>
#include <drivers/alloc.h>
#include <drivers/hvfs.h>
#include <drivers/net/nsock.h>
#include <drivers/tty.h>
#include <fs/vfs.h>
#include <fs/mnt.h>
#include <helpers/cwd.h>
#include <security/sks.h>
#include <hals/rtc.h>
#include <hals/ps2.h>
#include <hals/serial.h>
#include <hals/pci.h>
#include <sys/errno.h>
#include <uacpi/sleep.h>

#include "ioctl.h"

#define PATH_MAX 512

typedef struct {
    uint64_t key;
    int claimedlevel;
} permission;

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
#ifdef _GNU_SOURCE
    char domainname[65];
#endif
};

/* --- Global State Variables & External References --- */
bool fd_is_mnt[32] = {false};

extern char nodename[65];
extern uint64_t admin_key;
extern uint64_t user_key;
extern volatile uint64_t ticks;
extern volatile int last_scancode;
extern struct InterruptRegisters *current_intr;
extern struct process process_table[MAX_PROCESSES];
extern struct thread thread_table[MAX_THREADS];
extern volatile int current_thread_id;
extern pci_device_t *devices;
extern uint32_t devicecount;

/* --- Internal Utility Functions --- */

static uint64_t get_rsp(void) {
    uint64_t rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

static int resolve_vfs_path(const char *user_path, char *out_path, size_t max_size) {
    if (user_path == NULL || user_path[0] == '\0') {
        printk(LOG_WARNING, "[SYSCALL] resolve_vfs_path: Invalid or empty user path pointer.\n");
        return -EINVAL;
    }

    if (out_path == NULL || max_size == 0) {
        printk(LOG_ERROR, "[SYSCALL] resolve_vfs_path: Invalid output buffer destination.\n");
        return -EINVAL;
    }

    char combined[PATH_MAX];
    memset(combined, 0, PATH_MAX);

    if (user_path[0] == '/') {
        if (strlen(user_path) >= max_size) {
            printk(LOG_ERROR, "[SYSCALL] resolve_vfs_path: Absolute path exceeds max limit.\n");
            return -ENAMETOOLONG;
        }
        strncpy(combined, user_path, PATH_MAX - 1);
    } else {
        char *current_cwd = getpcwd();
        if (current_cwd == NULL) {
            printk(LOG_ERROR, "[SYSCALL] resolve_vfs_path: Unable to retrieve current working directory.\n");
            return -ENOENT;
        }

        size_t cwd_len = strlen(current_cwd);
        size_t path_len = strlen(user_path);

        if (cwd_len + 1 + path_len >= PATH_MAX) {
            printk(LOG_ERROR, "[SYSCALL] resolve_vfs_path: Combined relative path exceeds buffer limit.\n");
            return -ENAMETOOLONG;
        }

        strncpy(combined, current_cwd, PATH_MAX - 1);
        if (cwd_len > 0 && combined[cwd_len - 1] != '/') {
            combined[cwd_len] = '/';
            combined[cwd_len + 1] = '\0';
        }
        strncat(combined, user_path, PATH_MAX - strlen(combined) - 1);
    }

    int result = canonicalize_path(combined, out_path, max_size);
    if (result != 0) {
        printk(LOG_WARNING, "[SYSCALL] resolve_vfs_path: Path canonicalization failed for '%s'.\n", combined);
        return result;
    }

    return 0;
}

static char *sys_realpath_impl(const char *path, char *resolved_path) {
    char temp_buf[PATH_MAX];
    memset(temp_buf, 0, PATH_MAX);

    if (resolve_vfs_path(path, temp_buf, sizeof(temp_buf)) != 0) {
        printk(LOG_WARNING, "[SYSCALL] realpath_impl: Failed to resolve path string.\n");
        return NULL;
    }

    if (resolved_path != NULL) {
        strncpy(resolved_path, temp_buf, PATH_MAX - 1);
        resolved_path[PATH_MAX - 1] = '\0';
        return resolved_path;
    } else {
        size_t len = strlen(temp_buf) + 1;
        char *mem = (char *)kmalloc(len);
        if (mem == NULL) {
            printk(LOG_ERROR, "[SYSCALL] realpath_impl: Memory allocation failure for resolved path.\n");
            return NULL;
        }
        memcpy(mem, temp_buf, len);
        return mem;
    }
}

void set_tls(size_t key, uint64_t val) {
    if (key >= 16) {
        printk(LOG_ERROR, "[TLS] set_tls: Key index %size_t out of bounds.\n", key);
        return;
    }
    thread_table[current_thread_id].tls_slots[key] = val;
}

uint64_t get_tls(size_t key) {
    if (key >= 16) {
        printk(LOG_ERROR, "[TLS] get_tls: Key index %size_t out of bounds.\n", key);
        return 0;
    }
    return thread_table[current_thread_id].tls_slots[key];
}

/* --- System Call Dispatcher Handlers --- */

// Syscall 0: read
unsigned long long sys_read(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    uint64_t fd = a->arg[0];
    void *buf = (void *)a->arg[1];
    size_t count = (size_t)a->arg[2];
    uint64_t offset = a->arg[3];

    if (buf == NULL) {
        printk(LOG_ERROR, "[SYSCALL] sys_read: Passed buffer pointer is NULL.\n");
        return (unsigned long long)-EFAULT;
    }

    if (fd == 1 || fd == 2) {
        printk(LOG_WARNING, "[SYSCALL] sys_read: Attempted to read write-only descriptor %llu.\n", fd);
        return 0;
    }

    int tracking_idx = (int)(fd - 2);
    if (tracking_idx >= 0 && tracking_idx < 32 && fd_is_mnt[tracking_idx]) {
        return read(tracking_idx, buf, count, offset);
    }

    return vfs_read(tracking_idx, buf, count, offset);
}

// Syscall 1: write
unsigned long long sys_write(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    uint64_t fd = a->arg[0];
    const void *buf = (const void *)a->arg[1];
    size_t count = (size_t)a->arg[2];

    if (buf == NULL) {
        printk(LOG_ERROR, "[SYSCALL] sys_write: Passed buffer pointer is NULL.\n");
        return (unsigned long long)-EFAULT;
    }

    if (fd > 2) {
        int tracking_idx = (int)(fd - 2);
        if (tracking_idx >= 0 && tracking_idx < 32 && fd_is_mnt[tracking_idx]) {
            return write(tracking_idx, buf, count);
        }
        return vfs_write_file(tracking_idx, buf, count);
    } else if (fd == 1 || fd == 2) {
        const char *user_str = (const char *)buf;

        for (size_t i = 0; i < count; i++) {
            if (user_str[i] == '\n') {
                serial_write_char('\r');
            }
            serial_write_char(user_str[i]);
            tty_putchar(user_str[i]);
        }
        return count;
    }

    printk(LOG_ERROR, "[SYSCALL] sys_write: Invalid file descriptor %llu.\n", fd);
    return (unsigned long long)-EBADF;
}

// Syscall 2: open
unsigned long long sys_open(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    const char *user_path = (const char *)a->arg[0];
    int flags = (int)a->arg[1];
    uint32_t mode = (uint32_t)a->arg[2];

    char path[PATH_MAX];
    if (resolve_vfs_path(user_path, path, sizeof(path)) != 0) {
        printk(LOG_WARNING, "[SYSCALL] sys_open: Path resolution failed.\n");
        return (unsigned long long)-ENOENT;
    }

    bool is_dev = (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v');

    if (is_dev) {
        long fd = open((char *)path, flags, mode);
        if (fd < 0) {
            printk(LOG_ERROR, "[SYSCALL] sys_open: Device open failed for path '%s'.\n", path);
            return (unsigned long long)fd;
        }
        int tracking_idx = (int)fd;
        if (tracking_idx >= 0 && tracking_idx < 32) {
            fd_is_mnt[tracking_idx] = true;
        }
        return (unsigned long long)(fd + 2);
    } else {
        long fd = vfs_open(path, flags, mode);
        if (fd < 0) {
            printk(LOG_WARNING, "[SYSCALL] sys_open: VFS file open failed for path '%s', return: %d. flags: %x. mode: %x\n", path, fd, flags, mode);
            return (unsigned long long)fd;
        }
        int tracking_idx = (int)fd;
        if (tracking_idx >= 0 && tracking_idx < 32) {
            fd_is_mnt[tracking_idx] = false;
        }
        return (unsigned long long)(fd + 2);
    }
}

// Syscall 3: vfs_mkdir
unsigned long long sys_mkdir(arg *a) {
    if (a == NULL) {
        printk(LOG_ERROR, "[SYSCALL] sys_mkdir: NULL arg pointer\n");
        return (unsigned long long)-EINVAL;
    }

    const char *path = (const char *)a->arg[0];
    printk(LOG_WARNING, "[SYSCALL] sys_mkdir: mkdir called for '%s'\n", path ? path : "(null)");

    char path_buf[PATH_MAX];
    if (resolve_vfs_path(path, path_buf, sizeof(path_buf)) != 0) {
        printk(LOG_ERROR, "[SYSCALL] sys_mkdir: Path resolution failed for '%s'.\n", path ? path : "(null)");
        return (unsigned long long)-ENOENT;
    }

    int res = vfs_mkdir(path_buf, (uint32_t)a->arg[1]);
    if (res < 0) {
        printk(LOG_WARNING, "[SYSCALL] sys_mkdir: vfs_mkdir returned %d for '%s'\n", res, path_buf);
    } else {
        printk(LOG_INFO, "[SYSCALL] sys_mkdir: created '%s'\n", path_buf);
    }
    return (unsigned long long)res;
}

// Syscall 4: vfs_rmdir
unsigned long long sys_rmdir(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    char path_buf[PATH_MAX];
    if (resolve_vfs_path((const char *)a->arg[0], path_buf, sizeof(path_buf)) != 0) {
        printk(LOG_ERROR, "[SYSCALL] sys_rmdir: Path resolution failed.\n");
        return (unsigned long long)-ENOENT;
    }

    return (unsigned long long)vfs_rmdir(path_buf);
}

// Syscall 5: close
unsigned long long sys_close(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    uint64_t fd = a->arg[0];
    if (fd <= 2) {
        return 0; // Standard input/output/error descriptor handling
    }

    int tracking_idx = (int)(fd - 2);
    if (tracking_idx >= 0 && tracking_idx < 32) {
        fd_is_mnt[tracking_idx] = false;
    }

    return (unsigned long long)vfs_free_fd(tracking_idx);
}

// Syscall 6: vfs_move_file
unsigned long long sys_move_file(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    uint64_t fd = a->arg[0];
    if (fd <= 2) {
        printk(LOG_ERROR, "[SYSCALL] sys_move_file: Invalid standard descriptor pass.\n");
        return (unsigned long long)-EBADF;
    }

    char path_buf[PATH_MAX];
    if (resolve_vfs_path((const char *)a->arg[1], path_buf, sizeof(path_buf)) != 0) {
        printk(LOG_ERROR, "[SYSCALL] sys_move_file: Destination path resolution failed.\n");
        return (unsigned long long)-ENOENT;
    }

    return (unsigned long long)vfs_move_file((int)(fd - 2), path_buf);
}

// Syscall 7: create_file
unsigned long long sys_create_file(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    const char *raw_path = (const char *)a->arg[1];
    char path[PATH_MAX];

    if (resolve_vfs_path(raw_path, path, sizeof(path)) != 0) {
        printk(LOG_ERROR, "[SYSCALL] sys_create_file: Target file path resolution failed.\n");
        return (unsigned long long)-ENOENT;
    }

    bool is_dev = (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v');

    if (is_dev) {
        long status = create((char *)path);
        if (status < 0) {
            printk(LOG_ERROR, "[SYSCALL] sys_create_file: Device node creation failed.\n");
            return (unsigned long long)status;
        }
        int tracking_idx = (int)status;
        if (tracking_idx >= 0 && tracking_idx < 32) {
            fd_is_mnt[tracking_idx] = true;
        }
        return (unsigned long long)(status + 2);
    } else {
        long fd = vfs_create_file((void *)a->arg[0], path, (int)a->arg[2]);
        if (fd < 0) {
            printk(LOG_ERROR, "[SYSCALL] sys_create_file: VFS file creation failed.\n");
            return (unsigned long long)fd;
        }
        int tracking_idx = (int)fd;
        if (tracking_idx >= 0 && tracking_idx < 32) {
            fd_is_mnt[tracking_idx] = false;
        }
        return (unsigned long long)(fd + 2);
    }
}

// Syscall 8: vfs_delete_file
unsigned long long sys_delete_file(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    char path_buf[PATH_MAX];
    if (resolve_vfs_path((const char *)a->arg[0], path_buf, sizeof(path_buf)) != 0) {
        printk(LOG_ERROR, "[SYSCALL] sys_delete_file: Path resolution failed.\n");
        return (unsigned long long)-ENOENT;
    }

    return (unsigned long long)vfs_delete_file(path_buf);
}

// Syscall 9: get_permission_keys
unsigned long long sys_get_perm_key(arg *a) {
    if (a == NULL || a->arg[1] == 0) return (unsigned long long)-EINVAL;

    uint64_t target_level = a->arg[0];
    permission *perm_out = (permission *)a->arg[1];

    if (target_level == 0) {
        uint8_t signature[32];
        memset(signature, 0, 32);
        sign_key_with_pid((uint8_t *)&admin_key, sizeof(admin_key), getpid(), signature);

        permission perm = {
            .claimedlevel = 0,
            .key = signature_to_uint64_direct(signature)
        };
        memcpy(perm_out, &perm, sizeof(permission));
        return 0;
    } else if (target_level == 1) {
        uint8_t signature[32];
        memset(signature, 0, 32);
        sign_key_with_pid((uint8_t *)&user_key, sizeof(user_key), getpid(), signature);

        permission perm = {
            .claimedlevel = 1,
            .key = signature_to_uint64_direct(signature)
        };
        memcpy(perm_out, &perm, sizeof(permission));
        return 0;
    }

    printk(LOG_ERROR, "[SYSCALL] sys_get_perm_key: Invalid requested permission level %llu.\n", target_level);
    return (unsigned long long)-EACCES;
}

// Syscall 10: graduate
unsigned long long sys_graduate(arg *a) {
    (void)a;
    printk(LOG_INFO, "[SYSCALL] sys_graduate: Elevating privilege context.\n");
    graduate();
    return 0;
}

// Syscall 11: draw_rect
unsigned long long sys_draw_rect(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    draw_rect(a->arg[0], a->arg[1], a->arg[2], a->arg[3], a->arg[4], a->arg[5], a->arg[6]);
    return 0;
}

// Syscall 12: syscall_exit_handler
unsigned long long sys_exit_raw(arg *a, struct InterruptRegisters *intr) {
    (void)a;
    (void)intr;
    printk(LOG_ERROR, "[SYSCALL] Kernel exit handler executing for process terminated state.\n");
    return 0;
}

unsigned long long sys_exit_handler(arg *a) {
    return sys_exit_raw(a, current_intr);
}

// Syscall 13: ipc_recv
unsigned long long sys_ipc_recv(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    ipc_recv((void *)a->arg[0], a->arg[1], (uint32_t *)a->arg[2]);
    return 0;
}

// Syscall 14: ipc_send
unsigned long long sys_ipc_send(arg *a) {
    if (a == NULL || a->arg[1] == 0) return (unsigned long long)-EINVAL;

    ipc_send(a->arg[0], (void *)a->arg[1], a->arg[2]);
    return 0;
}

// Syscall 15: getpid
unsigned long long sys_getpid(arg *a) {
    (void)a;
    return (unsigned long long)getpid();
}

// Syscall 16: terminate
unsigned long long sys_terminate(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    uint64_t pid = a->arg[0];
    printk(LOG_INFO, "[SYSCALL] Terminating PID %llu.\n", pid);
    return (unsigned long long)terminate(pid, pid);
}

// Syscall 17: vfs_fstat
unsigned long long sys_fstat(arg *a) {
    if (a == NULL || a->arg[1] == 0) return (unsigned long long)-EINVAL;

    uint64_t fd = a->arg[0];
    struct vfs_stat *st = (struct vfs_stat *)a->arg[1];

    if (fd <= 2) {
        printk(LOG_ERROR, "[SYSCALL] sys_fstat: EBADF for standard descriptor %llu.\n", fd);
        return (unsigned long long)-EBADF;
    }

    return (unsigned long long)vfs_fstat((int)(fd - 2), st);
}

// Syscall 18: read_stdin_scancodes
unsigned long long sys_read_stdin(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    asm volatile("sti");

    uint8_t *user_buf = (uint8_t *)a->arg[0];
    uint64_t bytes_to_read = a->arg[1];

    if (bytes_to_read == 0 || user_buf == NULL) {
        return 0;
    }

    uint64_t bytes_read = 0;
    while (bytes_read < bytes_to_read) {
        while (last_scancode == -1) {
            asm volatile("hlt");
        }
        user_buf[bytes_read++] = (uint8_t)last_scancode;
        last_scancode = -1;
    }

    return bytes_read;
}

// Syscall 19: spawn
unsigned long long sys_spawn(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    char *path = (char *)a->arg[0];
    uint64_t flags = a->arg[1];
    char **argv = (char **)a->arg[2];
    char *envp = (char *)a->arg[3];

    return (unsigned long long)spawn(path, flags, argv, envp);
}

// Syscall 20: waitpid
unsigned long long sys_waitpid(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    return (unsigned long long)waitpid(a->arg[0]);
}

// Syscall 21: draw_image
unsigned long long sys_draw_image(arg *a) {
    if (a == NULL || a->arg[4] == 0) return (unsigned long long)-EINVAL;

    draw_image(a->arg[0], a->arg[1], a->arg[2], a->arg[3], (uint8_t *)a->arg[4]);
    return 0;
}

// Syscall 22: vmm_mmap
unsigned long long sys_mmap(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    void *addr = (void *)a->arg[0];
    size_t length = (size_t)a->arg[1];
    int prot = (int)a->arg[2];
    int flags = (int)a->arg[3];
    int fd = (int)a->arg[4];
    int64_t offset = (int64_t)a->arg[5];

    return (uint64_t)vmm_mmap(addr, length, prot, flags, fd, offset);
}

// Syscall 23: vmm_munmap
unsigned long long sys_munmap(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    return (unsigned long long)vmm_munmap((void *)a->arg[0], (size_t)a->arg[1]);
}

// Syscall 24: set_signal_handler
unsigned long long sys_set_signal_handler(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    return (unsigned long long)set_signal_handler((int)a->arg[0], a->arg[1]);
}

// Syscall 25: send_signal
unsigned long long sys_send_signal(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    return (unsigned long long)send_signal((int)a->arg[0], (int)a->arg[1]);
}

// Syscall 26: read_mouse
unsigned long long sys_read_mouse(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    return (unsigned long long)read_mouse((void *)a->arg[0], a->arg[1]);
}

// Syscall 27: get_pixel
unsigned long long sys_get_pixel(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    get_pixel(a->arg[0], a->arg[1], (uint8_t *)a->arg[2], (uint8_t *)a->arg[3], (uint8_t *)a->arg[4]);
    return 0;
}

// Syscall 28: ipc_send_nonblock
unsigned long long sys_ipc_send_nonblock(arg *a) {
    if (a == NULL || a->arg[1] == 0) return (unsigned long long)-EINVAL;

    ipc_send_nonblock(a->arg[0], (void *)a->arg[1], a->arg[2]);
    return 0;
}

// Syscall 29: ipc_recv_nonblock
unsigned long long sys_ipc_recv_nonblock(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    ipc_recv_nonblock((void *)a->arg[0], a->arg[1], (uint32_t *)a->arg[2]);
    return 0;
}

// Syscall 30: socket
unsigned long long sys_socket(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    return (unsigned long long)sock(a->arg[0], a->arg[1]);
}

// Syscall 31: socket_connect
unsigned long long sys_socket_connect(arg *a) {
    if (a == NULL || a->arg[1] == 0) return (unsigned long long)-EINVAL;

    uint64_t sock_idx = a->arg[0];
    struct net_socket sock = sockets[sock_idx];
    return (unsigned long long)sock.connect(&sock, (const char *)a->arg[1]);
}

// Syscall 32: socket_recv
unsigned long long sys_socket_recv(arg *a) {
    if (a == NULL || a->arg[1] == 0) return (unsigned long long)-EINVAL;

    uint64_t sock_idx = a->arg[0];
    struct net_socket sock = sockets[sock_idx];
    return (unsigned long long)sock.recv(&sock, (void *)a->arg[1], a->arg[2]);
}

// Syscall 33: socket_send
unsigned long long sys_socket_send(arg *a) {
    if (a == NULL || a->arg[1] == 0) return (unsigned long long)-EINVAL;

    uint64_t sock_idx = a->arg[0];
    struct net_socket sock = sockets[sock_idx];
    return (unsigned long long)sock.send(&sock, (void *)a->arg[1], a->arg[2]);
}

// Syscall 34: socket_close
unsigned long long sys_socket_close(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    uint64_t sock_idx = a->arg[0];
    struct net_socket sock = sockets[sock_idx];
    return (unsigned long long)sock.close(&sock);
}

// Syscall 35: get_ticks
unsigned long long sys_get_ticks(arg *a) {
    (void)a;
    return ticks * 10;
}

// Syscall 36: sleep_ms
unsigned long long sys_sleep_ms(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;
    // Execution yielding timer sleep delay disabled or unbacked
    return 0;
}

// Syscall 37: get_launchd_pid
unsigned long long sys_get_launchd_pid(arg *a) {
    (void)a;
    return (unsigned long long)get_launchd_pid();
}

// Syscall 38: uname
unsigned long long sys_uname(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    struct utsname *u = (struct utsname *)a->arg[0];
    memset(u, 0, sizeof(struct utsname));

    strncpy(u->sysname, "La Carrera", sizeof(u->sysname) - 1);
    strncpy(u->nodename, nodename, sizeof(u->nodename) - 1);
    strncpy(u->release, "4.5.0-rc1", sizeof(u->release) - 1);
    strncpy(u->version, "#1 NOSMP PREEMPT", sizeof(u->version) - 1);
    strncpy(u->machine, "x86_64", sizeof(u->machine) - 1);

    return 0;
}

// Syscall 39: sethostname
unsigned long long sys_sethostname(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    const char *new_name = (const char *)a->arg[0];
    size_t len = (size_t)a->arg[1];

    if (len >= sizeof(nodename)) {
        len = sizeof(nodename) - 1;
    }

    memset(nodename, 0, sizeof(nodename));
    strncpy(nodename, new_name, len);
    return 0;
}

// Syscall 40: gethostname
unsigned long long sys_gethostname(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    char *dest = (char *)a->arg[0];
    size_t len = (size_t)a->arg[1];

    memset(dest, 0, len);
    strncpy(dest, nodename, len - 1);
    return 0;
}

// Syscall 41: rtc_get_time
unsigned long long sys_rtc_get_time(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    rtc_get_time((struct timespec *)a->arg[0]);
    return 0;
}

// Syscall 42: vfs_listdir
unsigned long long sys_listdir(arg *a) {
    if (a == NULL || a->arg[1] == 0) return (unsigned long long)-EINVAL;

    char path_buf[PATH_MAX];
    if (resolve_vfs_path((const char *)a->arg[0], path_buf, sizeof(path_buf)) != 0) {
        printk(LOG_ERROR, "[SYSCALL] sys_listdir: Target path resolution failed.\n");
        return (unsigned long long)-ENOENT;
    }

    return (unsigned long long)vfs_listdir(path_buf, (char **)a->arg[1], a->arg[2]);
}

// Syscall 43: uacpi_reboot
unsigned long long sys_reboot(arg *a) {
    (void)a;
    printk(LOG_INFO, "[SYSCALL] Triggering systemic hardware reboot via ACPI.\n");
    uacpi_reboot();
    return 0;
}

// Syscall 44: poweroff
unsigned long long sys_poweroff(arg *a) {
    (void)a;
    printk(LOG_INFO, "[SYSCALL] System entering Sleep State S5 (Poweroff).\n");
    uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
    return 0;
}

// Syscall 45: vfs_delete_file (alias)
unsigned long long sys_delete_file_alias(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    char path_buf[PATH_MAX];
    if (resolve_vfs_path((const char *)a->arg[0], path_buf, sizeof(path_buf)) != 0) {
        printk(LOG_ERROR, "[SYSCALL] sys_delete_file_alias: Target path resolution failed.\n");
        return (unsigned long long)-ENOENT;
    }

    return (unsigned long long)vfs_delete_file(path_buf);
}

// Syscall 46: ioctl
unsigned long long sys_ioctl(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    return (unsigned long long)ioctl((int)a->arg[0], a->arg[1], (void *)a->arg[2]);
}

// Syscall 47: hvfs_create
unsigned long long sys_hvfs_create(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    return (unsigned long long)hvfs_create((char *)a->arg[0]);
}

// Syscall 48: hvfs_set_type
unsigned long long sys_hvfs_set_type(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    if (a->arg[1] == HVFS_TYPE_FUNCTION) {
        printk(LOG_WARNING, "[SYSCALL] sys_hvfs_set_type: Disallowed HVFS_TYPE_FUNCTION assignment.\n");
        return (unsigned long long)-ENOENT;
    }

    return (unsigned long long)hvfs_set_type((const char *)a->arg[0], a->arg[1]);
}

// Syscall 49: hvfs_set
unsigned long long sys_hvfs_set(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    return (unsigned long long)hvfs_set((const char *)a->arg[0], (const void *)a->arg[1], a->arg[2]);
}

// Syscall 50: hvfs_get
unsigned long long sys_hvfs_get(arg *a) {
    if (a == NULL || a->arg[0] == 0 || a->arg[1] == 0) return (unsigned long long)-EINVAL;

    return (unsigned long long)hvfs_get((const char *)a->arg[0], (void *)a->arg[1], a->arg[2]);
}

// Syscall 51: hvfs_get_type
unsigned long long sys_hvfs_get_type(arg *a) {
    if (a == NULL || a->arg[0] == 0 || a->arg[1] == 0) return (unsigned long long)-EINVAL;

    return (unsigned long long)hvfs_get_type((const char *)a->arg[0], (hvfs_type_t *)a->arg[1]);
}

// Syscall 52: hvfs_remove
unsigned long long sys_hvfs_remove(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    return (unsigned long long)hvfs_remove((const char *)a->arg[0]);
}

// Syscall 53: hvfs_listdir
unsigned long long sys_hvfs_listdir(arg *a) {
    if (a == NULL || a->arg[0] == 0 || a->arg[1] == 0) return (unsigned long long)-EINVAL;

    return (unsigned long long)hvfs_listdir((const char *)a->arg[0], (char *)a->arg[1], a->arg[2]);
}

// Syscall 54: hvfs_stat
unsigned long long sys_hvfs_stat(arg *a) {
    if (a == NULL || a->arg[0] == 0 || a->arg[1] == 0) return (unsigned long long)-EINVAL;

    return (unsigned long long)hvfs_stat((const char *)a->arg[0], (hvfs_stat_t *)a->arg[1]);
}

// Syscall 55: chdir
unsigned long long sys_chdir(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    char path_buf[PATH_MAX];
    if (resolve_vfs_path((const char *)a->arg[0], path_buf, sizeof(path_buf)) != 0) {
        printk(LOG_ERROR, "[SYSCALL] sys_chdir: Invalid path target.\n");
        return (unsigned long long)-ENOENT;
    }

    return (unsigned long long)chdir(path_buf);
}

// Syscall 56: getcwd
unsigned long long sys_getcwd(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    char *ret = getcwd((char *)a->arg[0], (size_t)a->arg[1]);
    return ret ? 0 : (unsigned long long)-1;
}

// Syscall 57: realpath
unsigned long long sys_realpath(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    const char *path = (const char *)a->arg[0];
    char *resolved_path = (char *)a->arg[1];

    char *res = sys_realpath_impl(path, resolved_path);
    if (res == NULL) {
        printk(LOG_ERROR, "[SYSCALL] sys_realpath: Path canonicalization implementation returned NULL.\n");
        return (unsigned long long)-ENOENT;
    }

    return (unsigned long long)res;
}

// Syscall 58: ps
unsigned long long sys_ps(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    return (unsigned long long)ps((struct utask *)a->arg[0], a->arg[1]);
}

// Syscall 59: TTY Clear
unsigned long long sys_tty_clear(arg *a) {
    (void)a;
    printk(LOG_TRACE, "[TTY] System clearing terminal display frame buffer.\n");
    tty_clear();
    return 0;
}

// Syscall 60: TTY switch
unsigned long long sys_tty_switch(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    tty_switch(a->arg[0]);
    return 0;
}

// Syscall 61: TTY pixel
unsigned long long sys_tty_draw_pixel(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    tty_draw_pixel(a->arg[0], a->arg[1], a->arg[2]);
    return 0;
}

// Syscall 62: TTY img
unsigned long long sys_tty_draw_img(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    int start_x = (int)a->arg[0];
    int start_y = (int)a->arg[1];
    unsigned int *img_buffer = (unsigned int *)a->arg[2];
    int w = (int)a->arg[3];
    int h = (int)a->arg[4];

    if (img_buffer == NULL || w <= 0 || h <= 0) {
        printk(LOG_ERROR, "[TTY] sys_tty_draw_img: Invalid parameters or image memory reference.\n");
        return 1;
    }

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            unsigned int color = img_buffer[row * w + col];
            tty_draw_pixel(start_x + col, start_y + row, color);
        }
    }

    return 0;
}

// Syscall 63: Draw rect (TTY)
unsigned long long sys_tty_draw_rect(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    uint64_t x = a->arg[0];
    uint64_t y = a->arg[1];
    uint64_t w = a->arg[2];
    uint64_t h = a->arg[3];
    uint32_t color = (uint32_t)a->arg[4];

    for (uint64_t iy = 0; iy < h; iy++) {
        for (uint64_t ix = 0; ix < w; ix++) {
            tty_draw_pixel(x + ix, y + iy, color);
        }
    }

    return 0;
}

// Syscall 64: TLS get
unsigned long long sys_get_tls(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    return get_tls((size_t)a->arg[0]);
}

// Syscall 65: TLS set
unsigned long long sys_set_tls(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    set_tls((size_t)a->arg[0], a->arg[1]);
    return 0;
}

// Syscall 66: clone
unsigned long long sys_clone(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    void *entry_point = (void *)a->arg[0];
    void *child_stack = (void *)a->arg[1];
    void *child_arg   = (void *)a->arg[2];

    if (entry_point == NULL || child_stack == NULL) {
        printk(LOG_ERROR, "[SYSCALL] sys_clone: Invalid execution entry point or stack location.\n");
        return (unsigned long long)-EINVAL;
    }

    return (unsigned long long)clone(entry_point, child_stack, child_arg, true);
}

// Syscall 67: join
unsigned long long sys_join(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    return (unsigned long long)sys_thread_join(a->arg[0], (int *)a->arg[1]);
}

// Syscall 68: pci lookup
unsigned long long sys_pci_lookup(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    pci_device_t *buf = (pci_device_t *)a->arg[0];
    uint64_t max_requested = a->arg[1];
    uint64_t used = 0;

    for (uint32_t i = 0; i < devicecount; i++) {
        if (used == max_requested) {
            break;
        }
        buf[used] = devices[i];
        used++;
    }

    return (unsigned long long)used;
}

// Syscall 69: sys thread exit
unsigned long long sysc_thread_exit(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    sys_thread_exit((int)a->arg[0]);
    return 0;
}

// Syscall 70: fork
unsigned long long sys_fork(arg *a) {
    (void)a;

    if (current_intr == NULL) {
        printk(LOG_ERROR, "[SYSCALL] sys_fork: Execution context interrupt registers vector NULL.\n");
        return (unsigned long long)-EINVAL;
    }

    return (unsigned long long)fork(current_intr);
}

// Syscall 71: execve
unsigned long long sys_execve(arg *a) {
    if (a == NULL || a->arg[0] == 0) return (unsigned long long)-EINVAL;

    if (current_intr == NULL) {
        printk(LOG_ERROR, "[SYSCALL] sys_execve: Execution context interrupt registers vector NULL.\n");
        return (unsigned long long)-EINVAL;
    }

    return (unsigned long long)execve((const char *)a->arg[0], (char *const *)a->arg[1], (char *const *)a->arg[2], current_intr->rsp);
}

// Syscall 72: link
unsigned long long sys_link(arg *a) {
    return vfs_link(a->arg[0], a->arg[1]);
}

// Syscall 73: socket_listen
unsigned long long sys_socket_listen(arg *a) {
    if (a == NULL || a->arg[1] == 0) {
        printk(LOG_ERROR, "[SYSCALL] sys_socket_listen: invalid arguments\n");
        return (unsigned long long)-EINVAL;
    }

    uint64_t sock_idx = a->arg[0];
    const char *addr = (const char *)a->arg[1];
    struct net_socket sock = sockets[sock_idx];
    printk(LOG_INFO, "[SYSCALL] sys_socket_listen: called sock idx %llu addr='%s'\n", (unsigned long long)sock_idx, addr ? addr : "(null)");
    if (sock.listen == NULL) {
        printk(LOG_ERROR, "[SYSCALL] sys_socket_listen: Protocol does not support listen().\n");
        return (unsigned long long)-EOPNOTSUPP;
    }

    int result = sock.listen(&sock, addr);
    sockets[sock_idx] = sock;
    return (unsigned long long)result;
}

// Syscall 74: socket_accept
unsigned long long sys_socket_accept(arg *a) {
    if (a == NULL) {
        printk(LOG_ERROR, "[SYSCALL] sys_socket_accept: NULL arg pointer\n");
        return (unsigned long long)-EINVAL;
    }

    uint64_t sock_idx = a->arg[0];
    printk(LOG_INFO, "[SYSCALL] sys_socket_accept: called sock idx %llu\n", (unsigned long long)sock_idx);
    struct net_socket sock = sockets[sock_idx];

    if (sock.accept == NULL) {
        printk(LOG_ERROR, "[SYSCALL] sys_socket_accept: Protocol does not support accept().\n");
        return (unsigned long long)-EOPNOTSUPP;
    }

    struct net_socket client;
    memset(&client, 0, sizeof(client));

    int new_idx = sock.accept(&sock, &client);
    if (new_idx < 0) {
        printk(LOG_WARNING, "[SYSCALL] sys_socket_accept: accept() returned failure.\n");
        return (unsigned long long)new_idx;
    }

    // sock's md (e.g. listening flag) may have changed as a side effect
    // of accept() blocking/polling; persist it back.
    sockets[sock_idx] = sock;

    return (unsigned long long)new_idx;
}

// Syscall 75: socket_bind
unsigned long long sys_socket_bind(arg *a) {
    if (a == NULL || a->arg[1] == 0) {
        printk(LOG_ERROR, "[SYSCALL] sys_socket_bind: invalid arguments\n");
        return (unsigned long long)-EINVAL;
    }

    uint64_t sock_idx = a->arg[0];
    const char *addr = (const char *)a->arg[1];
    struct net_socket sock = sockets[sock_idx];
    printk(LOG_INFO, "[SYSCALL] sys_socket_bind: called sock idx %llu addr='%s'\n", (unsigned long long)sock_idx, addr ? addr : "(null)");
    if (sock.bind == NULL) {
        printk(LOG_ERROR, "[SYSCALL] sys_socket_bind: Protocol does not support bind().\n");
        return (unsigned long long)-EOPNOTSUPP;
    }

    int result = sock.bind(&sock, addr);
    sockets[sock_idx] = sock;
    if (result < 0) {
        printk(LOG_WARNING, "[SYSCALL] sys_socket_bind: bind returned %d\n", result);
    }
    return (unsigned long long)result;
}