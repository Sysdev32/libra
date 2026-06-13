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
#endif
