// SPDX-License-Identifier: GPL-3.0-only
#include <drivers/fb.h>
#include <string.h>
#include <limine.h>

#define MAX_ORDER 11 
#define PAGE_SIZE 4096

// --- VMM DEFINITIONS & FLAGS ---
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_FRAME    0x000FFFFFFFFFF000ULL 
#define HHDM_OFFSET  0xffff800000000000ULL

#define PML4_INDEX(addr) (((addr) >> 39) & 0x1FF)
#define PDPT_INDEX(addr) (((addr) >> 30) & 0x1FF)
#define PD_INDEX(addr)   (((addr) >> 21) & 0x1FF)
#define PT_INDEX(addr)   (((addr) >> 12) & 0x1FF)

typedef uint64_t page_table_t;

// Limine Handshaking Blocks
__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

extern volatile struct limine_hhdm_request hhdm_request;

struct Page {
    uint8_t order;
    uint8_t is_free;
    struct Page *next;
    struct Page *prev;
};

// Global metadata trackers
struct Page *all_pages = NULL;
size_t total_page_count = 0;
uint64_t physical_mem_highest = 0;
struct Page *free_lists[MAX_ORDER] = {NULL};

// --- DOUBLY LINKED LIST HELPERS ---
static void pmm_free_list_add(int order, struct Page *page) {
    if (order < 0 || order >= MAX_ORDER || page == NULL) return;
    page->order = order;
    page->is_free = 1;
    page->next = free_lists[order];
    page->prev = NULL;
    if (free_lists[order] != NULL) {
        free_lists[order]->prev = page;
    }
    free_lists[order] = page;
}

static void pmm_free_list_remove(int order, struct Page *page) {
    if (order < 0 || order >= MAX_ORDER || page == NULL) return;
    if (free_lists[order] == page) {
        free_lists[order] = page->next;
    }
    if (page->prev != NULL) {
        page->prev->next = page->next;
    }
    if (page->next != NULL) {
        page->next->prev = page->prev;
    }
    page->next = NULL;
    page->prev = NULL;
    page->is_free = 0;
}

// --- PHYSICAL MEMORY MANAGER FUNCTIONS ---

void pmm_init(void) {
    struct limine_memmap_response *map = memmap_request.response;
    if (map == NULL) {
        for(;;);
    }

    // Step 1: Discover highest physical address boundary
    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *entry = map->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uint64_t top = entry->base + entry->length;
            if (top > physical_mem_highest) {
                physical_mem_highest = top;
            }
        }
    }

    total_page_count = physical_mem_highest / PAGE_SIZE;
    uint64_t array_size_bytes = total_page_count * sizeof(struct Page);
    uint64_t tracker_array_phys_addr = 0;

    // Step 2: Allocate internal metadata structure array safely via early boot carving
    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *entry = map->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= array_size_bytes) {
            tracker_array_phys_addr = entry->base;
            entry->base += array_size_bytes;
            entry->length -= array_size_bytes;
            break;
        }
    }

    all_pages = (struct Page *)(tracker_array_phys_addr + HHDM_OFFSET);

    // Default entire tracking index to unmapped space
    for (size_t i = 0; i < total_page_count; i++) {
        all_pages[i].is_free = 0; 
        all_pages[i].order = 0;
        all_pages[i].next = NULL;
        all_pages[i].prev = NULL;
    }

    // Step 3: Populate order 0 free list with remaining raw usable blocks
    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *entry = map->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uint64_t start_phys = entry->base;
            uint64_t end_phys = entry->base + entry->length;

            for (uint64_t addr = start_phys; addr < end_phys; addr += PAGE_SIZE) {
                uint64_t page_index = addr / PAGE_SIZE;

                // Skip the physical memory occupied by the tracker array itself
                if (addr >= tracker_array_phys_addr && addr < (tracker_array_phys_addr + array_size_bytes)) {
                    continue; 
                }

                // Explicitly prevent PMM from reclaiming hardcoded testing areas
                if (addr >= 0x8000000 && addr < 0xA000000) {
                    continue;
                }
                if (addr >= 0xA000000 && addr < 0xA001000) {
                    continue;
                }

                all_pages[page_index].is_free = 1;
                all_pages[page_index].order = 0;
                pmm_free_list_add(0, &all_pages[page_index]);
            }
        }
    }
}

void *pmm_alloc_pages(int order) {
    if (order < 0 || order >= MAX_ORDER) return NULL;

    for (int i = order; i < MAX_ORDER; i++) {
        if (free_lists[i] != NULL) {
            struct Page *block = free_lists[i];
            pmm_free_list_remove(i, block);

            // Buddy Allocator core splitting mechanics
            while (i > order) {
                i--;
                size_t block_index = block - all_pages;
                size_t buddy_index = block_index + (1 << i);
                struct Page *buddy = &all_pages[buddy_index];
                pmm_free_list_add(i, buddy);
            }

            block->is_free = 0;
            block->order = order;
            uint64_t phys_addr = (uint64_t)(block - all_pages) * PAGE_SIZE;
            return (void *)(phys_addr + HHDM_OFFSET);
        }
    }
    return NULL; // System Out of Memory
}

void pmm_free_pages(void *ptr, int order) {
    if (ptr == NULL || order < 0 || order >= MAX_ORDER) return;

    uint64_t virt_addr = (uint64_t)ptr;
    uint64_t phys_addr = virt_addr - HHDM_OFFSET;
    size_t block_index = phys_addr / PAGE_SIZE;
    struct Page *block = &all_pages[block_index];

    while (order < MAX_ORDER - 1) {
        size_t buddy_index = block_index ^ (1 << order);
        if (buddy_index >= total_page_count) break;

        struct Page *buddy = &all_pages[buddy_index];
        if (!buddy->is_free || buddy->order != order) break;

        pmm_free_list_remove(order, buddy);

        if (buddy_index < block_index) {
            block_index = buddy_index;
            block = buddy;
        }
        order++;
    }
    pmm_free_list_add(order, block);
}

// --- VIRTUAL MEMORY MANAGER FUNCTIONS ---

static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + HHDM_OFFSET);
}

// Internal tool for tree traversal routines to claim single physical sheets
static inline void *vmm_get_phys_page(void) {
    void *virt = pmm_alloc_pages(0);
    if (!virt) return NULL;
    return (void *)((uint64_t)virt - HHDM_OFFSET);
}

/**
 * Retrieves a virtual address pointer to Limine's current active root page table structure.
 */
page_table_t *vmm_get_current_pml4(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t pml4_phys = cr3 & PTE_FRAME;
    uint64_t pml4_virt = pml4_phys + HHDM_OFFSET;
    return (page_table_t *)pml4_virt;
}

/**
 * Creates an isolated, sandboxed user process address space context.
 * Copies kernel space mappings (entries 256-511) and immediately loads the context into CR3.
 * * @return Virtual address pointer to the newly created and active PML4 root table.
 */
page_table_t *vmm_create_address_space(void) {
    // 1. Request a clean physical frame for our root PML4 directory
    void *virt_page = pmm_alloc_pages(0);
    if (!virt_page) return NULL;
    
    memset(virt_page, 0, PAGE_SIZE);
    page_table_t *new_pml4 = (page_table_t *)virt_page;
    page_table_t *kernel_pml4 = vmm_get_current_pml4();
    
    // 2. Clone the higher-half kernel space maps (indices 256 to 511)
    // This isolates user space mappings completely while leaving kernel space intact
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = kernel_pml4[i];
    }
    
    // 3. Compute the raw physical address of our new root directory structure
    uint64_t new_pml4_phys = (uint64_t)new_pml4 - HHDM_OFFSET;
    
    // 4. Update the CR3 register to context switch into our newly isolated address space
    asm volatile("mov %0, %%cr3" :: "r"(new_pml4_phys) : "memory");
    
    return new_pml4;
}

void vmm_map_page(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    size_t pml4_idx = PML4_INDEX(virt);
    if (!(pml4[pml4_idx] & PTE_PRESENT)) {
        uint64_t new_table_phys = (uint64_t)vmm_get_phys_page();
        memset(phys_to_virt(new_table_phys), 0, PAGE_SIZE);
        pml4[pml4_idx] = new_table_phys | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    } else {
        // Safe traversal protection: Propagate PTE_USER if the child page mapping uses it
        if (flags & PTE_USER) pml4[pml4_idx] |= PTE_USER;
    }
    page_table_t *pdpt = phys_to_virt(pml4[pml4_idx] & PTE_FRAME);

    size_t pdpt_idx = PDPT_INDEX(virt);
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
        uint64_t new_table_phys = (uint64_t)vmm_get_phys_page();
        memset(phys_to_virt(new_table_phys), 0, PAGE_SIZE);
        pdpt[pdpt_idx] = new_table_phys | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    } else {
        if (flags & PTE_USER) pdpt[pdpt_idx] |= PTE_USER;
    }
    page_table_t *pd = phys_to_virt(pdpt[pdpt_idx] & PTE_FRAME);

    size_t pd_idx = PD_INDEX(virt);
    if (!(pd[pd_idx] & PTE_PRESENT)) {
        uint64_t new_table_phys = (uint64_t)vmm_get_phys_page();
        memset(phys_to_virt(new_table_phys), 0, PAGE_SIZE);
        pd[pd_idx] = new_table_phys | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    } else {
        if (flags & PTE_USER) pd[pd_idx] |= PTE_USER;
    }
    page_table_t *pt = phys_to_virt(pd[pd_idx] & PTE_FRAME);

    size_t pt_idx = PT_INDEX(virt);
    pt[pt_idx] = (phys & PTE_FRAME) | PTE_PRESENT | flags;

    // Flush the translation lookaside buffer (TLB) for this modified page
    asm volatile("invlpg (%0)" ::"r"(virt) : "memory");
}