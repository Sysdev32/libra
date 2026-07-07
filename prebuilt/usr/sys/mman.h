/* Minimal mmap/munmap header for Libra userspace
 * Provides POSIX-like prototypes and flags used by user programs.
 */

#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protections */
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

/* Flags */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* POSIX-like prototypes */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset);
int munmap(void *addr, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_MMAN_H */
