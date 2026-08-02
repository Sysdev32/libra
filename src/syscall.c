#include <systable.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <arch/x86_64/idt.h>
#include <string.h>
#include <drivers/fb.h>
#include <drivers/alloc.h>
#include <limine.h>
#include <arch/x86_64/schedule.h>
#include <drivers/hvfs.h>
#include <fs/vfs.h>
#include <uacpi/uacpi.h>
#include <uacpi/sleep.h>
#include <drivers/net/HTTP.h>
#include <drivers/net/IPV4.h>
#include <drivers/net/TCP.h>
#include <drivers/net/nsock.h>
#include <drivers/net/UDP.h>
#include <fs/mnt.h>
#include <hals/net/RTL8139.h>
#include <helpers/cwd.h>
#include <security/sks.h>

#include "drivers/tty.h"

static uint64_t get_rsp(void) {
    uint64_t rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}
typedef struct {
    uint64_t key;
    int claimedlevel;
} permission;

extern char nodename[65];
extern uint64_t admin_key;
extern uint64_t user_key;
#define PATH_MAX 512

// Helper to resolve user relative or absolute path against current_cwd and return realpath
static int resolve_vfs_path(const char *user_path, char *out_path, size_t max_size) {
    if (!user_path || user_path[0] == '\0') return -1;

    char combined[PATH_MAX];
    char* current_cwd = getpcwd();
    
    if (user_path[0] == '/') {
        if (strlen(user_path) >= max_size) return -1;
        strcpy(combined, user_path);
    } else {
        if (!current_cwd) return -1;
        size_t cwd_len = strlen(current_cwd);
        size_t path_len = strlen(user_path);

        if (cwd_len + 1 + path_len >= max_size) return -1;

        strcpy(combined, current_cwd);
        if (cwd_len > 0 && combined[cwd_len - 1] != '/') {
            combined[cwd_len] = '/';
            combined[cwd_len + 1] = '\0';
        }
        size_t end = strlen(combined);
        strcpy(combined + end, user_path);
    }

    return canonicalize_path(combined, out_path, max_size);
}

// System call level Realpath function wrapper
static char *sys_realpath_impl(const char *path, char *resolved_path) {
    char temp_buf[PATH_MAX];
    if (resolve_vfs_path(path, temp_buf, sizeof(temp_buf)) != 0) {
        return NULL;
    }

    if (resolved_path != NULL) {
        strcpy(resolved_path, temp_buf);
        return resolved_path;
    } else {
        char *mem = (char*)kmalloc(strlen(temp_buf) + 1);
        if (mem) {
            strcpy(mem, temp_buf);
        }
        return mem;
    }
}

bool fd_is_mnt[32] = {false};
extern volatile uint64_t ticks;
extern volatile int last_scancode;

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

// Syscall 0: read
unsigned long long sys_read(arg* a) {
    if (a->arg[0] == 1 || a->arg[0] == 2) {
        return 0;
    }
    int tracking_idx = (int)(a->arg[0] - 2);
    if (tracking_idx >= 0 && tracking_idx < 32 && fd_is_mnt[tracking_idx]) {
        return read(tracking_idx, (void*)a->arg[1], a->arg[2], a->arg[3]);
    }
    return vfs_read(tracking_idx, (void*)a->arg[1], a->arg[2], a->arg[3]);
}

// Syscall 1: write
unsigned long long sys_write(arg* a) {
    if (a->arg[0] > 2) {
        int tracking_idx = (int)(a->arg[0] - 2);
        if (tracking_idx >= 0 && tracking_idx < 32 && fd_is_mnt[tracking_idx]) {
            return write(tracking_idx, (void*)a->arg[1], a->arg[2]);
        }
        return vfs_write_file(tracking_idx, (void*)a->arg[1], a->arg[2]);
    } 
    else if (a->arg[0] == 1 || a->arg[0] == 2) {
        char *user_str = (char*)a->arg[1];
        unsigned long length = a->arg[2];

        for (unsigned long i = 0; i < length; i++) {
            if (user_str[i] == '\n')
                serial_write_char('\r');
            serial_write_char(user_str[i]);
            tty_putchar(user_str[i]);
        }
        return length;
    }
    return (unsigned long long)-1;
}

// Syscall 2: open
unsigned long long sys_open(arg* a) {
    const char* user_path = (const char*)a->arg[0];
    int flags = a->arg[1];
    uint32_t mode = a->arg[2];

    char path[PATH_MAX];
    if (resolve_vfs_path(user_path, path, sizeof(path)) != 0) {
        return (unsigned long long)-ENOENT;
    }

    bool is_dev = (path[0] == '/' && 
                   path[1] == 'd' && 
                   path[2] == 'e' && 
                   path[3] == 'v');

    if (is_dev) {
        long fd = open((char*)path, flags, mode);
        if (fd < 0) {
            return fd;
        } else {
            int tracking_idx = (int)fd;
            if (tracking_idx >= 0 && tracking_idx < 32) {
                fd_is_mnt[tracking_idx] = true;
            }
            return fd + 2;
        }
    } else {
        long fd = vfs_open(path, flags, mode);
        if (fd < 0) {
            return fd;
        } else {
            int tracking_idx = (int)fd;
            if (tracking_idx >= 0 && tracking_idx < 32) {
                fd_is_mnt[tracking_idx] = false;
            }
            return fd + 2;
        }
    }
}

// Syscall 3: vfs_mkdir
unsigned long long sys_mkdir(arg* a) {
    char path_buf[PATH_MAX];
    if (resolve_vfs_path((const char*)a->arg[0], path_buf, sizeof(path_buf)) != 0) {
        return (unsigned long long)-ENOENT;
    }
    return vfs_mkdir(path_buf, a->arg[1]);
}

// Syscall 4: vfs_rmdir
unsigned long long sys_rmdir(arg* a) {
    char path_buf[PATH_MAX];
    if (resolve_vfs_path((const char*)a->arg[0], path_buf, sizeof(path_buf)) != 0) {
        return (unsigned long long)-ENOENT;
    }
    return vfs_rmdir(path_buf);
}

// Syscall 5: close
unsigned long long sys_close(arg* a) {
    if (a->arg[0] <= 2) {
        return 0;
    } else {
        int tracking_idx = (int)(a->arg[0] - 2);
        if (tracking_idx >= 0 && tracking_idx < 32) {
            fd_is_mnt[tracking_idx] = false;
        }
        return vfs_free_fd(tracking_idx);
    }
}

// Syscall 6: vfs_move_file
unsigned long long sys_move_file(arg* a) {
    if (a->arg[0] <= 2) {
        return (unsigned long long)-1;
    } else {
        char path_buf[PATH_MAX];
        if (resolve_vfs_path((const char*)a->arg[1], path_buf, sizeof(path_buf)) != 0) {
            return (unsigned long long)-ENOENT;
        }
        return vfs_move_file(a->arg[0] - 2, path_buf);
    }
}

// Syscall 7: create_file
unsigned long long sys_create_file(arg* a) {
    const char* raw_path = (const char*)a->arg[1];
    char path[PATH_MAX];

    if (resolve_vfs_path(raw_path, path, sizeof(path)) != 0) {
        return (unsigned long long)-ENOENT;
    }

    bool is_dev = (path[0] == '/' && 
                   path[1] == 'd' && 
                   path[2] == 'e' && 
                   path[3] == 'v');

    if (is_dev) {
        long status = create((char*)path);
        if (status < 0) {
            return status;
        } else {
            int tracking_idx = (int)status;
            if (tracking_idx >= 0 && tracking_idx < 32) {
                fd_is_mnt[tracking_idx] = true;
            }
            return status + 2;
        }
    } else {
        long fd = vfs_create_file((void*)a->arg[0], path, a->arg[2]);
        if (fd < 0) {
            return fd;
        } else {
            int tracking_idx = (int)fd;
            if (tracking_idx >= 0 && tracking_idx < 32) {
                fd_is_mnt[tracking_idx] = false;
            }
            return fd + 2;
        }
    }
}

// Syscall 8: vfs_delete_file
unsigned long long sys_delete_file(arg* a) {
    char path_buf[PATH_MAX];
    if (resolve_vfs_path((const char*)a->arg[0], path_buf, sizeof(path_buf)) != 0) {
        return (unsigned long long)-ENOENT;
    }
    return vfs_delete_file(path_buf);
}

// Syscall 9: get_permission_keys
unsigned long long sys_get_perm_key(arg* a) {
    if (a->arg[0] == 0) {
        uint8_t signature[32];
        sign_key_with_pid((uint8_t*)admin_key, sizeof(admin_key), getpid(), signature); 
        permission perm = { .claimedlevel = 0, .key = signature_to_uint64_direct(signature)};
        memcpy((void*)a->arg[1], &perm, sizeof(permission));
    } else if (a->arg[0] == 1) {
        uint8_t signature[32];
        sign_key_with_pid((uint8_t*)user_key, sizeof(user_key), getpid(), signature); 
        permission perm = { .claimedlevel = 0, .key = signature_to_uint64_direct(signature)};
        memcpy((void*)a->arg[1], &perm, sizeof(permission));
    }
    return 0;
}

// Syscall 10: graduate
unsigned long long sys_graduate(arg* a) {
    graduate();
    return 0;
}

// Syscall 11: draw_rect
unsigned long long sys_draw_rect(arg* a) {
    draw_rect(a->arg[0], a->arg[1], a->arg[2], a->arg[3], a->arg[4], a->arg[5], a->arg[6]);
    return 0;
}

// Syscall 12: syscall_exit_handler
unsigned long long sys_exit_handler(arg* a) {
    syscall_exit_handler(get_rsp(), a->arg[0]);
    return 0;
}

// Syscall 13: ipc_recv
unsigned long long sys_ipc_recv(arg* a) {
    ipc_recv((void*)a->arg[0], a->arg[1], (uint32_t*)a->arg[2]);
    return 0;
}

// Syscall 14: ipc_send
unsigned long long sys_ipc_send(arg* a) {
    ipc_send(a->arg[1], (void*)a->arg[2], a->arg[3]);
    return 0;
}

// Syscall 15: getpid
unsigned long long sys_getpid(arg* a) {
    return getpid();
}

// Syscall 16: terminate
unsigned long long sys_terminate(arg* a) {
    return (unsigned long long)terminate((void*)a->arg[0], a->arg[0]);
}

// Syscall 17: vfs_fstat
unsigned long long sys_fstat(arg* a) {
    if (a->arg[0] <= 2) {
        return (unsigned long long)-EBADF;
    } else {
        return vfs_fstat(a->arg[0] - 2, (struct vfs_stat*)a->arg[1]);
    }
}

// Syscall 18: read_stdin_scancodes
unsigned long long sys_read_stdin(arg* a) {
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
unsigned long long sys_spawn(arg* a) {
    return spawn((char*)a->arg[0], a->arg[1], (char**)a->arg[2], (char*)a->arg[3]);
}

// Syscall 20: waitpid
unsigned long long sys_waitpid(arg* a) {
    return waitpid(a->arg[0]);
}

// Syscall 21: draw_image
unsigned long long sys_draw_image(arg* a) {
    draw_image(a->arg[0], a->arg[1], a->arg[2], a->arg[3], (uint8_t*)a->arg[4]);
    return 0;
}

// Syscall 22: vmm_mmap
unsigned long long sys_mmap(arg* a) {
    return (uint64_t)vmm_mmap((void*)a->arg[0], a->arg[1], (int)a->arg[2], (int)a->arg[3], (int)a->arg[4], (int64_t)a->arg[5]);
}

// Syscall 23: vmm_munmap
unsigned long long sys_munmap(arg* a) {
    return vmm_munmap((void*)a->arg[0], a->arg[1]);
}

// Syscall 24: set_signal_handler
unsigned long long sys_set_signal_handler(arg* a) {
    return set_signal_handler((int)a->arg[0], a->arg[1]);
}

// Syscall 25: send_signal
unsigned long long sys_send_signal(arg* a) {
    return send_signal((int)a->arg[0], (int)a->arg[1]);
}

// Syscall 26: read_mouse
unsigned long long sys_read_mouse(arg* a) {
    return read_mouse((void*)a->arg[0], a->arg[1]);
}

// Syscall 27: get_pixel
unsigned long long sys_get_pixel(arg* a) {
    get_pixel(a->arg[0], a->arg[1], (uint8_t*)a->arg[2], (uint8_t*)a->arg[3], (uint8_t*)a->arg[4]);
    return 0;
}

// Syscall 28: ipc_send_nonblock
unsigned long long sys_ipc_send_nonblock(arg* a) {
    ipc_send_nonblock(a->arg[0], (void*)a->arg[1], a->arg[2]);
    return 0;
}

// Syscall 29: ipc_recv_nonblock
unsigned long long sys_ipc_recv_nonblock(arg* a) {
    ipc_recv_nonblock((void*)a->arg[0], a->arg[1], a->arg[2]);
    return 0;
}

// Syscall 30: socket
unsigned long long sys_socket(arg* a) {
    return sock(a->arg[0], a->arg[1]);
}

// Syscall 31: socket_connect
unsigned long long sys_socket_connect(arg* a) {
    struct net_socket sock = sockets[a->arg[0]];
    return sock.connect(&sock, a->arg[1]);
}

// Syscall 32: socket_recv
unsigned long long sys_socket_recv(arg* a) {
    struct net_socket sock = sockets[a->arg[0]];
    return sock.recv(&sock, (void*)a->arg[1], a->arg[2]);
}

// Syscall 33: socket_send
unsigned long long sys_socket_send(arg* a) {
    struct net_socket sock = sockets[a->arg[0]];
    return sock.send(&sock, (void*)a->arg[1], a->arg[2]);
}

// Syscall 34: socket_close
unsigned long long sys_socket_close(arg* a) {
    struct net_socket sock = sockets[a->arg[0]];
    return sock.close(&sock);
}

// Syscall 35: get_ticks
unsigned long long sys_get_ticks(arg* a) {
    return ticks * 10;
}

// Syscall 36: sleep_ms
unsigned long long sys_sleep_ms(arg* a) {
    sleep_ms(a->arg[0]);
    return 0;
}

// Syscall 37: get_launchd_pid
unsigned long long sys_get_launchd_pid(arg* a) {
    return get_launchd_pid();
}

// Syscall 38: uname
unsigned long long sys_uname(arg* a) {
    struct utsname* u = (struct utsname*)a->arg[0];
    strcpy(u->machine, "x86_64");
    strcpy(u->nodename, nodename);
    strcpy(u->release, "4.5.0-rc1");
    strcpy(u->sysname, "La Carrera");
    strcpy(u->version, "#1 NOSMP PREEMPT");
    return 0;
}

// Syscall 39: sethostname
unsigned long long sys_sethostname(arg* a) {
    memset(nodename, 0, strlen(nodename));
    strncpy(nodename, (char*)a->arg[0], a->arg[1]);
    return 0;
}

// Syscall 40: gethostname
unsigned long long sys_gethostname(arg* a) {
    strncpy((char*)a->arg[0], nodename, a->arg[1]);
    return 0;
}

// Syscall 41: rtc_get_time
unsigned long long sys_rtc_get_time(arg* a) {
    rtc_get_time((struct timespec*)a->arg[0]);
    return 0;
}

// Syscall 42: vfs_listdir
unsigned long long sys_listdir(arg* a) {
    char path_buf[PATH_MAX];
    if (resolve_vfs_path((const char*)a->arg[0], path_buf, sizeof(path_buf)) != 0) {
        return (unsigned long long)-ENOENT;
    }
    return vfs_listdir(path_buf, (char**)a->arg[1], a->arg[2]);
}

// Syscall 43: uacpi_reboot
unsigned long long sys_reboot(arg* a) {
    uacpi_reboot();
    return 0;
}

// Syscall 44: poweroff
unsigned long long sys_poweroff(arg* a) {
    uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
    return 0;
}

// Syscall 45: vfs_delete_file (alias)
unsigned long long sys_delete_file_alias(arg* a) {
    char path_buf[PATH_MAX];
    if (resolve_vfs_path((const char*)a->arg[0], path_buf, sizeof(path_buf)) != 0) {
        return (unsigned long long)-ENOENT;
    }
    return vfs_delete_file(path_buf);
}

// Syscall 46: ioctl
unsigned long long sys_ioctl(arg* a) {
    return ioctl(a->arg[0], a->arg[1], (void*)a->arg[2]);
}

// Syscall 47: hvfs_create
unsigned long long sys_hvfs_create(arg* a) {
    return hvfs_create((char*)a->arg[0]);
}

// Syscall 48: hvfs_set_type
unsigned long long sys_hvfs_set_type(arg* a) {
    if (a->arg[1] == HVFS_TYPE_FUNCTION) {
        return (unsigned long long)-ENOENT;
    }
    return hvfs_set_type((const char*)a->arg[0], a->arg[1]);
}

// Syscall 49: hvfs_set
unsigned long long sys_hvfs_set(arg* a) {
    return hvfs_set((const char*)a->arg[0], (const void*)a->arg[1], a->arg[2]);
}

// Syscall 50: hvfs_get
unsigned long long sys_hvfs_get(arg* a) {
    return hvfs_get((const char*)a->arg[0], (void*)a->arg[1], a->arg[2]);
}

// Syscall 51: hvfs_get_type
unsigned long long sys_hvfs_get_type(arg* a) {
    return hvfs_get_type((const char*)a->arg[0], (hvfs_type_t*)a->arg[1]);
}

// Syscall 52: hvfs_remove
unsigned long long sys_hvfs_remove(arg* a) {
    return hvfs_remove((const char*)a->arg[0]);
}

// Syscall 53: hvfs_listdir
unsigned long long sys_hvfs_listdir(arg* a) {
    return hvfs_listdir((const char*)a->arg[0], (char*)a->arg[1], a->arg[2]);
}

// Syscall 54: hvfs_stat
unsigned long long sys_hvfs_stat(arg* a) {
    return hvfs_stat((const char*)a->arg[0], (hvfs_stat_t*)a->arg[1]);
}

// Syscall 55: chdir
unsigned long long sys_chdir(arg* a) {
    char path_buf[PATH_MAX];
    if (resolve_vfs_path((const char*)a->arg[0], path_buf, sizeof(path_buf)) != 0) {
        return (unsigned long long)-ENOENT;
    }
    return chdir(path_buf);
}

// Syscall 56: getcwd
unsigned long long sys_getcwd(arg* a) {
    char *ret = getcwd((char*)a->arg[0], (size_t)a->arg[1]);
    return ret ? 0 : (unsigned long long)-1;
}

// Syscall 57: realpath
unsigned long long sys_realpath(arg* a) {
    const char *path = (const char*)a->arg[0];
    char *resolved_path = (char*)a->arg[1];
    
    char *res = sys_realpath_impl(path, resolved_path);
    if (!res) {
        return (unsigned long long)-ENOENT;
    }
    return (unsigned long long)res;
}

// Syscall 58: ps
unsigned long long sys_ps(arg *a) {
    return ps((struct utask*)a->arg[0], a->arg[1]);
}

// Syscall 59: TTY Clear
unsigned long long sys_tty_clear(arg *a) {
    printk(LOG_TRACE, "bing bang t bang bing t bang bing y bang bang c bang bing l bang bong e bang bing a bing bang r\n");
    tty_clear();
    return 0;
}

// Syscall 60: TTY switch
unsigned long long sys_tty_switch(arg *a) {
    tty_switch(a->arg[0]);
    return 0;
}

// Syscall 61: TTY pixel
// Syscall: Draw single pixel
unsigned long long sys_tty_draw_pixel(arg *a) {
    tty_draw_pixel(a->arg[0], a->arg[1], a->arg[2]);
    return 0; // Success
}

// Syscall 62: TTY img
unsigned long long sys_tty_draw_img(arg *a) {
    int start_x = (int)a->arg[0];
    int start_y = (int)a->arg[1];
    unsigned int *img_buffer = (unsigned int *)a->arg[2]; // Assuming 32-bit color
    int w = (int)a->arg[3];
    int h = (int)a->arg[4];

    // Pointer/bounds check
    if (!img_buffer || w <= 0 || h <= 0) {
        return 1; // Return error code for invalid argument
    }

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            // Index into flat 1D pixel array: (row * width) + column
            unsigned int color = img_buffer[row * w + col];

            // Draw at the offset target coordinates
            tty_draw_pixel(start_x + col, start_y + row, color);
        }
    }

    return 0; // Success
}

// Syscall 63: Draw rect (TTY)
unsigned long long sys_tty_draw_rect(arg *a) {
    uint64_t x = a->arg[0];
    uint64_t y = a->arg[1];
    uint64_t w = a->arg[2];
    uint64_t h = a->arg[3];
    uint32_t color = a->arg[4];

    for (uint64_t iy = 0; iy < h; iy++) {
        for (uint64_t ix = 0; ix < w; ix++) {
            tty_draw_pixel(x + ix, y + iy, color);
        }
    }

    return 0;
}