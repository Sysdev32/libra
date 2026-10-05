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
#include <hals/virtio/virtio_gpu_accel.h>
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

// Syscall 0: read// Syscall 0: read
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
        return (unsigned long long)-EBADF;
    }

    int tracking_idx = (int)(fd - 2);
    if (tracking_idx < 0) {
        return (unsigned long long)-EBADF;
    }

    // Call open-subsystem read wrapper directly; falls back to VFS internally
    int res = read(tracking_idx, buf, count, offset);
    return (res < 0) ? (unsigned long long)res : (unsigned long long)res;
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
        // Direct write dispatcher that delegates between mountpoints, devfs, and VFS fallback
        int res = write(tracking_idx, buf, count);
        return (res < 0) ? (unsigned long long)res : (unsigned long long)res;
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

    // Call open() directly. Mount resolution and VFS fallback will handle routing.
    int fd = open((char *)path, flags, mode);
    if (fd < 0) {
        printk(LOG_WARNING, "[SYSCALL] sys_open: Open failed for path '%s', return: %d, flags: %x, mode: %x\n", path, fd, flags, mode);
        return (unsigned long long)fd;
    }

    int tracking_idx = fd;
    if (tracking_idx >= 0 && tracking_idx < 32) {
        fd_is_mnt[tracking_idx] = true;
    }

    // Shift by 2 to reserve 0 (stdin), 1 (stdout), and 2 (stderr) for user space
    return (unsigned long long)(fd + 2);
}

// Syscall 3: mkdir
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

    // Delegate directory creation through mount wrapper
    int res = mnt_mkdir(path_buf, (uint32_t)a->arg[1]);
    if (res < 0) {
        printk(LOG_WARNING, "[SYSCALL] sys_mkdir: mnt_mkdir returned %d for '%s'\n", res, path_buf);
    } else {
        printk(LOG_INFO, "[SYSCALL] sys_mkdir: created '%s'\n", path_buf);
    }
    return (unsigned long long)res;
}
// Syscall 4: rmdir
unsigned long long sys_rmdir(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    char path_buf[PATH_MAX];
    if (resolve_vfs_path((const char *)a->arg[0], path_buf, sizeof(path_buf)) != 0) {
        printk(LOG_ERROR, "[SYSCALL] sys_rmdir: Path resolution failed.\n");
        return (unsigned long long)-ENOENT;
    }

    // Direct routing to mountpoint directory remover
    return (unsigned long long)mnt_rmdir(path_buf);
}

// Syscall 5: close
unsigned long long sys_close(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    uint64_t fd = a->arg[0];
    if (fd <= 2) {
        return 0; // Standard input/output/error descriptors
    }

    int tracking_idx = (int)(fd - 2);
    if (tracking_idx >= 0 && tracking_idx < 32) {
        fd_is_mnt[tracking_idx] = false;
    }

    // Call open-subsystem close wrapper (handles both devfs/mount entries and VFS descriptor deallocation)
    return (unsigned long long)close(tracking_idx);
}

// Syscall 6: move_file
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

    int tracking_idx = (int)(fd - 2);
    return (unsigned long long)vfs_move_file(tracking_idx, path_buf);
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

    // Unified call to create(). Mount resolution falls back to vfs_create_file automatically.
    long status = create((char *)path);
    if (status < 0) {
        printk(LOG_ERROR, "[SYSCALL] sys_create_file: Node creation failed for path '%s'.\n", path);
        return (unsigned long long)status;
    }

    int tracking_idx = (int)status;
    if (tracking_idx >= 0 && tracking_idx < 32) {
        fd_is_mnt[tracking_idx] = true;
    }

    // Shift descriptor for user space
    return (unsigned long long)(status + 2);
}

// Syscall 8: delete_file
unsigned long long sys_delete_file(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    char path_buf[PATH_MAX];
    if (resolve_vfs_path((const char *)a->arg[0], path_buf, sizeof(path_buf)) != 0) {
        printk(LOG_ERROR, "[SYSCALL] sys_delete_file: Path resolution failed.\n");
        return (unsigned long long)-ENOENT;
    }

    // Delegate node deletion through mnt_unlink
    return (unsigned long long)mnt_unlink(path_buf);
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

    return (unsigned long long)mnt_listdir(path_buf, (char **)a->arg[1], a->arg[2]);
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
    tty_flush();
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

    tty_draw_image(start_x, start_y, (uint32_t)w, (uint32_t)h, img_buffer);
    return 0;
}

// Syscall 63: Draw rect (TTY)
unsigned long long sys_tty_draw_rect(arg *a) {
    if (a == NULL) return (unsigned long long)-EINVAL;

    int x = (int)a->arg[0];
    int y = (int)a->arg[1];
    int w = (int)a->arg[2];
    int h = (int)a->arg[3];
    uint32_t color = (uint32_t)a->arg[4];

    if (w <= 0 || h <= 0) {
        return (unsigned long long)-EINVAL;
    }

    tty_draw_rect(x, y, w, h, color);
    tty_flush();
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
    return -EINVAL;

    uint64_t sock_idx = a->arg[0];
    printk(LOG_INFO, "[SYSCALL] sys_socket_accept: called sock idx %llu\n", (unsigned long long)sock_idx);
    struct net_socket sock = sockets[sock_idx];

    if (sock.accept == NULL) {
        printk(LOG_ERROR, "[SYSCALL] sys_socket_accept: Protocol does not support accept().\n");
        return (unsigned long long)-EOPNOTSUPP;
    }

    struct net_socket client;
    memset(&client, 0, sizeof(client));
    printk(LOG_DEBUG, "memsetting client to null\n");

    int new_idx = sock.accept(&sock, &client);
    printk(LOG_DEBUG, "ran func, sock type: %d\n", sock.protocol);
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
typedef struct {
    drive_type_t backend;
    FormattingSystem scheme;
    bool opened;
    int partition_count;
    int partitions[16];
    int id;
} disk;
typedef struct {
    int disk_count;
    disk disks[16];
} dtable;
extern initialized_drive init_drives[32];
extern int init_drives_count;
// Syscall 76: sys_dtable
unsigned long long sys_dtable(arg *a) {
    dtable* buffer = (dtable*)a->arg[0];
    buffer->disk_count = init_drives_count;
    for (int i=0; i<init_drives_count; i++) {
        buffer->disks[i].backend = init_drives[i].drive.type;
        buffer->disks[i].scheme = init_drives[i].formatType;
        buffer->disks[i].id = i;
        buffer->disks[i].opened = false;
    }
}

// Syscall 77: sys_dopen
unsigned long long sys_dopen(arg *a) {
    dtable* buffer = (dtable*)a->arg[0];
    int disk_id = a->arg[1];
    int index = -1;
    for (int i=0; i<buffer->disk_count && i<16; i++) {
        if (buffer->disks[i].id == disk_id) {
            index = i;
            break;
        }
    }
    if (index == -1) return index;
    int partition_count = init_drives[buffer->disks[index].id].format.gpt_partition_table.count;
    init_volume_t* vols = get_vol_array();
    int* vol_count = get_vol_counter();
    for (int i=0; i<partition_count; i++) {
        init_volume_t volume = init_drives[buffer->disks[index].id].format.gpt_partition_table.vols[i];
        vols[(*vol_count)++] = volume;
        buffer->disks[index].partitions[buffer->disks[index].partition_count++] = *vol_count;
    }
    buffer->disks[index].opened = true;
    return 0;
}
typedef struct {
    FileSystem st_fs;
    uint64_t st_size;
    char st_name[36];
} dpstat;

typedef struct {
    FormattingSystem st_format;
    drive_type_t drive;
    uint64_t st_total_sectors;
    uint64_t st_sector_size;
} ddstat;

// Syscall 78: sys_dpstat
unsigned long long sys_dpstat(arg *a) {
    dtable* d_table = (dtable*)a->arg[0];
    int disk_id = a->arg[1];
    int partition_id = a->arg[2];
    dpstat* buf = (dpstat*)a->arg[3];
    init_volume_t* vol = &(get_vol_array()[d_table->disks[disk_id].partitions[partition_id]]);
    buf->st_fs = vol->fsType;
    buf->st_size = vol->fs.filesystem.vol->total_sectors * vol->fs.filesystem.vol->drive.sector_size;
    strncpy(buf->st_name, vol->base.name, 36);
    return 0;
}

// Syscall 79: sys_ddstat
unsigned long long sys_ddstat(arg *a) {
    dtable* d_table = (dtable*)a->arg[0];
    int disk_id = a->arg[1];
    ddstat* buf = (ddstat*)a->arg[3];
    initialized_drive* drive = &init_drives[d_table->disks[disk_id].id];
    buf->st_format = drive->formatType;
    buf->st_sector_size = drive->drive.sector_size;
    buf->drive = drive->drive.type;
    buf->st_total_sectors = drive->drive.total_sectors;
    return 0;
}
typedef struct {
    char name[64];
    int integer;
} hashmap_entry;
hashmap_entry mount_table[256];
int mount_count = 0;
// Syscall 80: sys_mount
unsigned long long sys_mount(arg *a) {
    if (mount_count == 256) return -1;
    dtable* d_table = (dtable*)a->arg[0];
    int disk_id = a->arg[1];
    char* name = (char*)a->arg[2];
    char* path = (char*)a->arg[3];
    initialized_drive* drive = &init_drives[d_table->disks[disk_id].id];
    partition_table_t table = gpt_parse_partitions(&drive->drive);
    for (int i=0; i<table.count; i++) {
        if (!strcmp(table.partitions[i]->name, name)) {
            hashmap_entry e;
            strncpy(e.name, path, 64);
            e.integer = mount(drive, i, path);
            mount_table[mount_count++] = e;
            return 0;
        }
    }
    return -1;
}

// Syscall 81: sys_umount
unsigned long long sys_umount(arg *a) {
    char* path = (char*)a->arg[0];
    for (int i=0; i<mount_count; i++) {
        if (!strcmp(path, mount_table[i].name)) {
            umount(mount_table[i].integer);
            return 0;
        }
    }
    return -1;
}

// Syscall 82: virtio_gpu_accel_create_context
//
// Args:
//   arg[0] = local_id - Process-local ID used to reference the context.
//   arg[1] = name     - Pointer to the context name string.
//
// Returns:
//   Context ID on success, -1 on failure.

extern virtio_gpu_device_t g_virtio_gpu;
extern virtio_gpu_accel_t g_virtio_gpu_accel;

#define MAX_CTX_ENTRIES 512

typedef struct {
    int ctx_id;
    int local_id;
    int owner_pid;
    bool active;
} ctx_entry_t;

ctx_entry_t ctx_entries[MAX_CTX_ENTRIES];

/*
 * Free-slot stack.
 *
 * Initially every slot is free.
 * free_ctx_count tells us how many slots are available.
 */
int free_ctx_slots[MAX_CTX_ENTRIES] = {
    511, 510, 509, 508, 507, 506, 505, 504,
    503, 502, 501, 500, 499, 498, 497, 496,
    495, 494, 493, 492, 491, 490, 489, 488,
    487, 486, 485, 484, 483, 482, 481, 480,
    479, 478, 477, 476, 475, 474, 473, 472,
    471, 470, 469, 468, 467, 466, 465, 464,
    463, 462, 461, 460, 459, 458, 457, 456,
    455, 454, 453, 452, 451, 450, 449, 448,
    447, 446, 445, 444, 443, 442, 441, 440,
    439, 438, 437, 436, 435, 434, 433, 432,
    431, 430, 429, 428, 427, 426, 425, 424,
    423, 422, 421, 420, 419, 418, 417, 416,
    415, 414, 413, 412, 411, 410, 409, 408,
    407, 406, 405, 404, 403, 402, 401, 400,
    399, 398, 397, 396, 395, 394, 393, 392,
    391, 390, 389, 388, 387, 386, 385, 384,
    383, 382, 381, 380, 379, 378, 377, 376,
    375, 374, 373, 372, 371, 370, 369, 368,
    367, 366, 365, 364, 363, 362, 361, 360,
    359, 358, 357, 356, 355, 354, 353, 352,
    351, 350, 349, 348, 347, 346, 345, 344,
    343, 342, 341, 340, 339, 338, 337, 336,
    335, 334, 333, 332, 331, 330, 329, 328,
    327, 326, 325, 324, 323, 322, 321, 320,
    319, 318, 317, 316, 315, 314, 313, 312,
    311, 310, 309, 308, 307, 306, 305, 304,
    303, 302, 301, 300, 299, 298, 297, 296,
    295, 294, 293, 292, 291, 290, 289, 288,
    287, 286, 285, 284, 283, 282, 281, 280,
    279, 278, 277, 276, 275, 274, 273, 272,
    271, 270, 269, 268, 267, 266, 265, 264,
    263, 262, 261, 260, 259, 258, 257, 256,
    255, 254, 253, 252, 251, 250, 249, 248,
    247, 246, 245, 244, 243, 242, 241, 240,
    239, 238, 237, 236, 235, 234, 233, 232,
    231, 230, 229, 228, 227, 226, 225, 224,
    223, 222, 221, 220, 219, 218, 217, 216,
    215, 214, 213, 212, 211, 210, 209, 208,
    207, 206, 205, 204, 203, 202, 201, 200,
    199, 198, 197, 196, 195, 194, 193, 192,
    191, 190, 189, 188, 187, 186, 185, 184,
    183, 182, 181, 180, 179, 178, 177, 176,
    175, 174, 173, 172, 171, 170, 169, 168,
    167, 166, 165, 164, 163, 162, 161, 160,
    159, 158, 157, 156, 155, 154, 153, 152,
    151, 150, 149, 148, 147, 146, 145, 144,
    143, 142, 141, 140, 139, 138, 137, 136,
    135, 134, 133, 132, 131, 130, 129, 128,
    127, 126, 125, 124, 123, 122, 121, 120,
    119, 118, 117, 116, 115, 114, 113, 112,
    111, 110, 109, 108, 107, 106, 105, 104,
    103, 102, 101, 100, 99, 98, 97, 96,
    95, 94, 93, 92, 91, 90, 89, 88,
    87, 86, 85, 84, 83, 82, 81, 80,
    79, 78, 77, 76, 75, 74, 73, 72,
    71, 70, 69, 68, 67, 66, 65, 64,
    63, 62, 61, 60, 59, 58, 57, 56,
    55, 54, 53, 52, 51, 50, 49, 48,
    47, 46, 45, 44, 43, 42, 41, 40,
    39, 38, 37, 36, 35, 34, 33, 32,
    31, 30, 29, 28, 27, 26, 25, 24,
    23, 22, 21, 20, 19, 18, 17, 16,
    15, 14, 13, 12, 11, 10, 9, 8,
    7, 6, 5, 4, 3, 2, 1, 0
};

int free_ctx_count = MAX_CTX_ENTRIES;


// Finds a context belonging to a specific process using its local ID.
//
// Args:
//   local_id  - Process-local ID of the context to find.
//   owner_pid - PID of the process that owns the context.
//
// Returns:
//   Global context ID on success, -1 if no matching context exists.
int lookup_entries(int local_id, int owner_pid) {
    for (int i = 0; i < MAX_CTX_ENTRIES; i++) {
        if (ctx_entries[i].active &&
            ctx_entries[i].local_id == local_id &&
            ctx_entries[i].owner_pid == owner_pid) {
            return ctx_entries[i].ctx_id;
        }
    }

    return -1;
}


// Syscall 82: virtio_gpu_accel_create_context
//
// Args:
//   arg[0] = local_id - Process-local ID used to reference the context.
//   arg[1] = name     - Pointer to the context name string.
//
// Returns:
//   Context ID on success, -1 on failure.
unsigned long long sys_vgpu_create_context(arg *a) {
    if (free_ctx_count == 0) {
        return -1;
    }

    int slot = free_ctx_slots[--free_ctx_count];

    ctx_entry_t *ctx_entry = &ctx_entries[slot];

    ctx_entry->ctx_id = g_virtio_gpu_accel.next_ctx_id++;
    ctx_entry->local_id = a->arg[0];
    ctx_entry->owner_pid = getpid();
    ctx_entry->active = true;

    unsigned long long result =
        virtio_gpu_accel_create_context(
            &g_virtio_gpu_accel,
            ctx_entry->ctx_id,
            (char *)a->arg[1]
        );

    if (result == 0) {
        ctx_entry->active = false;
        free_ctx_slots[free_ctx_count++] = slot;
        return -1;
    }

    return result;
}


// Syscall 83: virtio_gpu_accel_destroy_context
//
// Args:
//   arg[0] = local_id - Process-local ID of the context to destroy.
//
// Returns:
//   0 on success, -1 on failure.
unsigned long long sys_vgpu_destroy_context(arg *a) {
    int owner_pid = getpid();
    int local_id = a->arg[0];

    for (int i = 0; i < MAX_CTX_ENTRIES; i++) {
        ctx_entry_t *ctx_entry = &ctx_entries[i];

        if (!ctx_entry->active ||
            ctx_entry->local_id != local_id ||
            ctx_entry->owner_pid != owner_pid) {
            continue;
        }

        if (virtio_gpu_accel_destroy_context(
                &g_virtio_gpu_accel,
                ctx_entry->ctx_id) == 0) {
            return -1;
        }

        ctx_entry->active = false;
        free_ctx_slots[free_ctx_count++] = i;

        return 0;
    }

    return -1;
}


// Syscall 84: virtio_gpu_accel_create_3d_resource
//
// Args:
//   arg[0] = resource_id - ID of the resource to create.
//   arg[1] = target      - Target/type of the resource.
//   arg[2] = format      - Format of the resource.
//   arg[3] = width       - Width of the resource.
//   arg[4] = height      - Height of the resource.
//   arg[5] = depth       - Depth of the resource.
//
// Returns:
//   Result from virtio_gpu_accel_create_3d_resource().
unsigned long long sys_vgpu_create_resource(arg *a) {
    return virtio_gpu_accel_create_3d_resource(
        &g_virtio_gpu_accel,
        a->arg[0],
        a->arg[1],
        a->arg[2],
        a->arg[3],
        a->arg[4],
        a->arg[5]
    );
}


// Syscall 85: virtio_gpu_accel_attach_resource
//
// Args:
//   arg[0] = resource_id - ID of the resource to attach.
//
// Returns:
//   Result from virtio_gpu_accel_attach_resource(), or -1 if the
//   context/resource lookup fails.
unsigned long long sys_vgpu_attach_resource(arg *a) {
    int lookup = lookup_entries(a->arg[0], getpid());
    if (lookup == -1) return -1;

    return virtio_gpu_accel_attach_resource(
        &g_virtio_gpu_accel,
        lookup,
        a->arg[0]
    );
}


// Syscall 86: virtio_gpu_accel_detach_resource
//
// Args:
//   arg[0] = resource_id - ID of the resource to detach.
//
// Returns:
//   Result from virtio_gpu_accel_detach_resource(), or -1 if the
//   context/resource lookup fails.
unsigned long long sys_vgpu_detach_resource(arg *a) {
    int lookup = lookup_entries(a->arg[0], getpid());
    if (lookup == -1) return -1;

    return virtio_gpu_accel_detach_resource(
        &g_virtio_gpu_accel,
        lookup,
        a->arg[0]
    );
}


// Syscall 87: virtio_gpu_accel_submit_3d
//
// Args:
//   arg[0] = context_id   - Process-local ID of the context to submit to.
//   arg[1] = command      - Pointer to the 3D command buffer.
//   arg[2] = command_size - Size of the command buffer in bytes.
//
// Returns:
//   Result from virtio_gpu_accel_submit_3d(), or -1 if the
//   context lookup fails.
unsigned long long sys_vgpu_submit_3d(arg *a) {
    int lookup = lookup_entries(a->arg[0], getpid());
    if (lookup == -1) return -1;

    return virtio_gpu_accel_submit_3d(
        &g_virtio_gpu_accel,
        lookup,
        (const void *)a->arg[1],
        a->arg[2]
    );
}