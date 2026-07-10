// SPDX-License-Identifier: GPL-3.0-only
#include <drivers/alloc.h>
#include <string.h>
#include <stdint.h>

// Forward declarations of your existing physical subsystem
extern void pmm_init(void);
extern void *pmm_alloc_pages(int order);
extern void pmm_free_pages(void *ptr, int order);

#define HEAP_BLOCK_MAGIC 0xDEADC0DE
#define PAGE_SIZE 4096

// Meta block prefix sitting immediately before the returned data pointer
struct HeapBlock {
    uint32_t magic;
    uint32_t is_free;
    size_t size;
    struct HeapBlock *next;
};

// Global root pointer to our heap list mapping
static struct HeapBlock *heap_root = NULL;

// --- UNIFIED INITIALIZATION FUNCTION ---
void memory_init(void) {
    // 1. Parse Limine memory maps and index physical pages
    pmm_init();

    // 2. We skip any custom vmm_init() calls here.
    // By keeping Limine's native page tables active, your stack 
    // and framebuffer remain perfectly valid without triggering page faults.

    // 3. Pre-seed our heap with an initial 4KB page configuration bucket
    void *initial_page = pmm_alloc_pages(0);
    if (initial_page != NULL) {
        memset(initial_page, 0, PAGE_SIZE);
        
        heap_root = (struct HeapBlock *)initial_page;
        heap_root->magic = HEAP_BLOCK_MAGIC;
        heap_root->is_free = 1;
        heap_root->size = PAGE_SIZE - sizeof(struct HeapBlock);
        heap_root->next = NULL;
    }
}

// --- CORE ALLOCATOR ENGINE ---

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    // Align allocations to 8-byte word boundaries for hardware processing efficiency
    size = (size + 7) & ~7;

    struct HeapBlock *curr = heap_root;
    struct HeapBlock *best_fit = NULL;

    // Scan the tracking list for an available matching slot
    while (curr != NULL) {
        if (curr->magic == HEAP_BLOCK_MAGIC && curr->is_free && curr->size >= size) {
            if (best_fit == NULL || curr->size < best_fit->size) {
                best_fit = curr;
            }
        }
        curr = curr->next;
    }

    // Out of space in existing heap? Claim a completely fresh 4KB page from PMM
    if (best_fit == NULL) {
        void *new_page = pmm_alloc_pages(0);
        if (new_page == NULL) return NULL; // Absolute System Out Of Memory

        memset(new_page, 0, PAGE_SIZE);
        struct HeapBlock *new_block = (struct HeapBlock *)new_page;
        new_block->magic = HEAP_BLOCK_MAGIC;
        new_block->is_free = 1;
        new_block->size = PAGE_SIZE - sizeof(struct HeapBlock);
        new_block->next = NULL;

        // Append new page bucket to the end of the heap chain
        if (heap_root == NULL) {
            heap_root = new_block;
        } else {
            curr = heap_root;
            while (curr->next != NULL) {
                curr = curr->next;
            }
            curr->next = new_block;
        }
        best_fit = new_block;
    }

    // Split the block if the remaining space is large enough to warrant a new descriptor
    if (best_fit->size >= size + sizeof(struct HeapBlock) + 8) {
        struct HeapBlock *split_block = (struct HeapBlock *)((uintptr_t)best_fit + sizeof(struct HeapBlock) + size);
        split_block->magic = HEAP_BLOCK_MAGIC;
        split_block->is_free = 1;
        split_block->size = best_fit->size - size - sizeof(struct HeapBlock);
        split_block->next = best_fit->next;

        best_fit->size = size;
        best_fit->next = split_block;
    }

    best_fit->is_free = 0;

    // Return the address context pointing immediately *after* the header prefix
    return (void *)((uintptr_t)best_fit + sizeof(struct HeapBlock));
}

void kfree(void *ptr) {
    if (ptr == NULL) return;

    // Shift pointer backward to locate the tracking meta header structure
    struct HeapBlock *block = (struct HeapBlock *)((uintptr_t)ptr - sizeof(struct HeapBlock));
    
    // Safety verification check against memory corruption
    if (block->magic != HEAP_BLOCK_MAGIC) {
        return; 
    }

    block->is_free = 1;

    // De-fragmentation Pass: Merge contiguous free nodes to eliminate tracking holes
    struct HeapBlock *curr = heap_root;
    while (curr != NULL) {
        while (curr->is_free && curr->next != NULL && curr->next->is_free) {
            // Verify structural consistency before combining
            if (curr->next->magic == HEAP_BLOCK_MAGIC) {
                curr->size += sizeof(struct HeapBlock) + curr->next->size;
                curr->next = curr->next->next;
            } else {
                break;
            }
        }
        curr = curr->next;
    }
}

void *kcalloc(size_t num, size_t size) {
    size_t total = num * size;
    void *ptr = kmalloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *krealloc(void *ptr, size_t new_size) {
    if (ptr == NULL) {
        return kmalloc(new_size);
    }
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    struct HeapBlock *block = (struct HeapBlock *)((uintptr_t)ptr - sizeof(struct HeapBlock));
    if (block->magic != HEAP_BLOCK_MAGIC) {
        return NULL;
    }

    // If existing block already accommodates requested dimensions, keep using it
    if (block->size >= new_size) {
        return ptr;
    }

    // Otherwise, allocate a fresh target region and copy contents
    void *new_ptr = kmalloc(new_size);
    if (new_ptr != NULL) {
        memcpy(new_ptr, ptr, block->size);
        kfree(ptr);
    }
    return new_ptr;
}
