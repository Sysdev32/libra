/*
systable.h

This file includes the syscall table for Libra.
*/
#pragma once
#include <stdint.h>
typedef struct {
    uint64_t arg[8];
} arg;

typedef unsigned long long (*syscall)(arg* a);

// Syscall 0
unsigned long long sys_read(arg* a);

// Syscall 1
unsigned long long sys_write(arg* a);

// Syscall 2
unsigned long long sys_open(arg* a);

// Syscall 3
unsigned long long sys_mkdir(arg* a);

// Syscall 4
unsigned long long sys_rmdir(arg* a);

// Syscall 5
unsigned long long sys_close(arg* a);

// Syscall 6
unsigned long long sys_move_file(arg* a);

// Syscall 7
unsigned long long sys_create_file(arg* a);

// Syscall 8
unsigned long long sys_delete_file(arg* a);

// Syscall 9
unsigned long long sys_get_perm_key(arg* a);

// Syscall 10
unsigned long long sys_graduate(arg* a);

// Syscall 11
unsigned long long sys_draw_rect(arg* a);

// Syscall 12
unsigned long long sys_exit_handler(arg* a);

// Syscall 13
unsigned long long sys_ipc_recv(arg* a);

// Syscall 14
unsigned long long sys_ipc_send(arg* a);

// Syscall 15
unsigned long long sys_getpid(arg* a);

// Syscall 16
unsigned long long sys_terminate(arg* a);

// Syscall 17
unsigned long long sys_fstat(arg* a);

// Syscall 18
unsigned long long sys_read_stdin(arg* a);

// Syscall 19
unsigned long long sys_spawn(arg* a);

// Syscall 20
unsigned long long sys_waitpid(arg* a);

// Syscall 21
unsigned long long sys_draw_image(arg* a);

// Syscall 22
unsigned long long sys_mmap(arg* a);

// Syscall 23
unsigned long long sys_munmap(arg* a);

// Syscall 24
unsigned long long sys_set_signal_handler(arg* a);

// Syscall 25
unsigned long long sys_send_signal(arg* a);

// Syscall 26
unsigned long long sys_read_mouse(arg* a);

// Syscall 27
unsigned long long sys_get_pixel(arg* a);

// Syscall 28
unsigned long long sys_ipc_send_nonblock(arg* a);

// Syscall 29
unsigned long long sys_ipc_recv_nonblock(arg* a);

// Syscall 30
unsigned long long sys_socket(arg* a);

// Syscall 31
unsigned long long sys_socket_connect(arg* a);

// Syscall 32
unsigned long long sys_socket_recv(arg* a);

// Syscall 33
unsigned long long sys_socket_send(arg* a);

// Syscall 34
unsigned long long sys_socket_close(arg* a);

// Syscall 35
unsigned long long sys_get_ticks(arg* a);

// Syscall 36
unsigned long long sys_sleep_ms(arg* a);

// Syscall 37
unsigned long long sys_get_launchd_pid(arg* a);

// Syscall 38
unsigned long long sys_uname(arg* a);

// Syscall 39
unsigned long long sys_sethostname(arg* a);

// Syscall 40
unsigned long long sys_gethostname(arg* a);

// Syscall 41
unsigned long long sys_rtc_get_time(arg* a);

// Syscall 42
unsigned long long sys_listdir(arg* a);

// Syscall 43
unsigned long long sys_reboot(arg* a);

// Syscall 44
unsigned long long sys_poweroff(arg* a);

// Syscall 45
unsigned long long sys_delete_file_alias(arg* a);

// Syscall 46
unsigned long long sys_ioctl(arg* a);

// Syscall 47
unsigned long long sys_hvfs_create(arg* a);

// Syscall 48
unsigned long long sys_hvfs_set_type(arg* a);

// Syscall 49
unsigned long long sys_hvfs_set(arg* a);

// Syscall 50
unsigned long long sys_hvfs_get(arg* a);

// Syscall 51
unsigned long long sys_hvfs_get_type(arg* a);

// Syscall 52
unsigned long long sys_hvfs_remove(arg* a);

// Syscall 53
unsigned long long sys_hvfs_listdir(arg* a);

// Syscall 54
unsigned long long sys_hvfs_stat(arg* a);

// Syscall 55
unsigned long long sys_chdir(arg* a);

// Syscall 56
unsigned long long sys_getcwd(arg* a);

// Syscall 57
unsigned long long sys_realpath(arg* a);

// Syscall 58
unsigned long long sys_ps(arg *a);

// Syscall 59
unsigned long long sys_tty_clear(arg *a);

// Syscall 60
unsigned long long sys_tty_switch(arg *a);

// Syscall 61
unsigned long long sys_tty_draw_pixel(arg *a);

// Syscall 62
unsigned long long sys_tty_draw_img(arg *a);

// Syscall 63
unsigned long long sys_tty_draw_rect(arg *a);

static const syscall systable[64] = {
    sys_read, // Standard POSIX read
    sys_write, // Standard POSIX write
    sys_open, // Standard POSIX open
    sys_mkdir, // Standard POSIX mkdir
    sys_rmdir, // Standard POSIX rmdir
    sys_close, // Standard POSIX close
    sys_move_file,
    sys_create_file,
    sys_delete_file,
    sys_get_perm_key,
    sys_graduate, // Graduates (deprecated, old, do not use, ABI filler)
    sys_draw_rect, // Draw Image recommended
    sys_exit_handler, // DO NOT USE UNTIL RELEASE (undefined still sadly)
    sys_ipc_recv, // Work In Progress
    sys_ipc_send, // Work In Progress
    sys_getpid,
    sys_terminate, // working
    sys_fstat, // working
    sys_read_stdin, // working
    sys_spawn, // working
    sys_waitpid, // do not use
    sys_draw_image, // encouraged
    sys_mmap, // untested
    sys_munmap, // untested
    sys_set_signal_handler, // untested
    sys_send_signal, // untested
    sys_read_mouse, // untested
    sys_get_pixel, // working
    sys_ipc_send_nonblock, // WIP
    sys_ipc_recv_nonblock, // WIP
    sys_socket, // WIP
    sys_socket_connect, // WIP
    sys_socket_recv, // WIP
    sys_socket_send, // WIP
    sys_socket_close, // WIP
    sys_get_ticks, // returns ms since PIC init
    sys_sleep_ms,
    sys_get_launchd_pid, // Gets launchd pid
    sys_uname,
    sys_sethostname,
    sys_gethostname,
    sys_rtc_get_time,
    sys_listdir,
    sys_reboot,
    sys_poweroff,
    sys_delete_file_alias, // filler
    sys_ioctl, 
    // hvfs
    sys_hvfs_create,
    sys_hvfs_set_type,
    sys_hvfs_set,
    sys_hvfs_get,
    sys_hvfs_get_type,
    sys_hvfs_remove,
    sys_hvfs_listdir,
    sys_hvfs_stat,
    // Paths management
    sys_chdir,
    sys_getcwd,
    sys_realpath,
    sys_ps,
    sys_tty_clear,
    sys_tty_switch,
    sys_tty_draw_pixel,
    sys_tty_draw_img,
    sys_tty_draw_rect
};