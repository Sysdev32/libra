// SPDX-License-Identifier: GPL-3.0-only
#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>
typedef uint64_t page_table_t;
// Unified Memory Initialization
void memory_init(void);

// Core Heap Allocation API
void *kmalloc(size_t size);
void  kfree(void *ptr);
void *kcalloc(size_t num, size_t size);
void *krealloc(void *ptr, size_t new_size);
page_table_t *vmm_create_address_space(void);
void vmm_map_page(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
void *vmm_mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset);
int vmm_munmap(void *addr, size_t length);
#endif
