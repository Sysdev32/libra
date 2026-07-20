#include <stdint.h>
#include <stddef.h>
#include <fs/mnt.h>
#define BIT(x) (1ULL << (x))

extern full_t fd_table[32];
extern devfs_file files[64];
int ioctl(int fd, unsigned long request, void* arg) {
    if (fd_table[fd].mountpoint.part.type == DEVFS) {
        if (fd_table[fd].file.bitmask & DEVFS_IOCTL) {
            return fd_table[fd].file.ioctl(fd_table[fd].file.tty, request, arg);
        }
    }
    return -1;
}
