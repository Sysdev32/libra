// SPDX-License-Identifier: GPL-3.0-only
#include <drivers/fb.h>
#include <string.h>
#include <limine.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_ORDER 11 
#define PAGE_SIZE 4096

// --- VMM DEFINITIONS & FLAGS ---
#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITABLE   (1ULL << 1)
#define PTE_USER       (1ULL << 2)
#define PTE_NO_EXECUTE (1ULL << 63)
#define PTE_FRAME      0x000FFFFFFFFFF000ULL 
#define HHDM_OFFSET    0xffff800000000000ULL

#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define PROT_EXEC      0x4

#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

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

static void pmm_init_region(uint64_t base, uint64_t length) {
    uint64_t start_phys = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t end_phys = (base + length) & ~(PAGE_SIZE - 1);

    if (start_phys >= end_phys) return;

    for (uint64_t addr = start_phys; addr < end_phys; ) {
        size_t page_idx = addr / PAGE_SIZE;

        // Skip page zero permanently (IVT/BIOS)
        if (addr == 0) {
            addr += PAGE_SIZE;
            continue;
        }

        // Calculate maximum power-of-two order aligned to 'addr' and boundary limits
        int order = MAX_ORDER - 1;
        while (order > 0) {
            uint64_t block_size = (1ULL << order) * PAGE_SIZE;
            if ((addr % block_size == 0) && (addr + block_size <= end_phys)) {
                break;
            }
            order--;
        }

        // Ensure no page inside this block spans reserved ranges
        uint64_t block_size = (1ULL << order) * PAGE_SIZE;
        bool has_reserved = false;

        for (uint64_t check = addr; check < addr + block_size; check += PAGE_SIZE) {
            if ((check >= 0x8000000 && check < 0xA000000) ||
                (check >= 0xA000000 && check < 0xA001000)) {
                has_reserved = true;
                break;
            }
        }

        if (has_reserved) {
            // Downscale to Order 0 and mark non-reserved pages safely
            if ((addr < 0x8000000 || addr >= 0xA001000) &&
                !(addr >= 0xA000000 && addr < 0xA001000)) {
                all_pages[page_idx].is_free = 1;
                all_pages[page_idx].order = 0;
                pmm_free_list_add(0, &all_pages[page_idx]);
            }
            addr += PAGE_SIZE;
        } else {
            // Register naturally aligned high-order buddy block
            for (size_t i = 0; i < (1ULL << order); i++) {
                all_pages[page_idx + i].is_free = 1;
                all_pages[page_idx + i].order = order;
            }
            pmm_free_list_add(order, &all_pages[page_idx]);
            addr += block_size;
        }
    }
}

void pmm_init(void) {
    struct limine_memmap_response *map = memmap_request.response;
    if (map == NULL) {
        for (;;);
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

    // Step 2: Allocate internal metadata structure array via early boot carving
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

    // Step 3: Populate free lists with naturally-aligned, maximum-size buddy regions
    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *entry = map->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            pmm_init_region(entry->base, entry->length);
        }
    }
}

void *pmm_alloc_pages(int order) {
    if (order < 0 || order >= MAX_ORDER) return NULL;

    for (int i = order; i < MAX_ORDER; i++) {
        if (free_lists[i] != NULL) {
            struct Page *block = free_lists[i];
            pmm_free_list_remove(i, block);

            while (i > order) {
                i--;
                size_t block_index = block - all_pages;
                size_t buddy_index = block_index + (1 << i);
                struct Page *buddy = &all_pages[buddy_index];

                buddy->order = i;
                buddy->is_free = 1;
                pmm_free_list_add(i, buddy);
            }

            block->is_free = 0;
            block->order = order;
            uint64_t phys_addr = (uint64_t)(block - all_pages) * PAGE_SIZE;

            return (void *)(phys_addr + HHDM_OFFSET);
        }
    }
    return NULL;
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

/**
 * @brief Universal Helper Allocator
 * Converts required byte size to the minimum power-of-two buddy order
 * and allocates a contiguous physical region.
 */
void *helpalloc(size_t size_bytes, int *out_order) {
    if (size_bytes == 0) return NULL;

    size_t pages_needed = (size_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    int order = 0;

    while ((1UL << order) < pages_needed) {
        order++;
    }

    if (order >= MAX_ORDER) {
        return NULL;
    }

    if (out_order) {
        *out_order = order;
    }

    return pmm_alloc_pages(order);
}

// --- VIRTUAL MEMORY MANAGER FUNCTIONS ---

static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + HHDM_OFFSET);
}

static inline void *vmm_get_phys_page(void) {
    void *virt = pmm_alloc_pages(0);
    if (!virt) return NULL;
    return (void *)((uint64_t)virt - HHDM_OFFSET);
}

page_table_t *vmm_get_current_pml4(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t pml4_phys = cr3 & PTE_FRAME;
    uint64_t pml4_virt = pml4_phys + HHDM_OFFSET;
    return (page_table_t *)pml4_virt;
}

page_table_t *vmm_create_address_space(void) {
    void *virt_page = pmm_alloc_pages(0);
    if (!virt_page) return NULL;
    
    memset(virt_page, 0, PAGE_SIZE);
    page_table_t *new_pml4 = (page_table_t *)virt_page;
    page_table_t *kernel_pml4 = vmm_get_current_pml4();
    
    // Copy upper-half kernel space maps (indices 256 to 511)
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = kernel_pml4[i];
    }
    
    return new_pml4;
}

void vmm_map_page(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    size_t pml4_idx = PML4_INDEX(virt);
    
    // Prevent user flags from inadvertently escalating shared kernel-space PML4 entries
    uint64_t table_user_flag = (pml4_idx < 256) ? (flags & PTE_USER) : 0;

    if (!(pml4[pml4_idx] & PTE_PRESENT)) {
        uint64_t new_table_phys = (uint64_t)vmm_get_phys_page();
        memset(phys_to_virt(new_table_phys), 0, PAGE_SIZE);
        pml4[pml4_idx] = new_table_phys | PTE_PRESENT | PTE_WRITABLE | table_user_flag;
    } else {
        if (table_user_flag) pml4[pml4_idx] |= PTE_USER;
    }
    page_table_t *pdpt = phys_to_virt(pml4[pml4_idx] & PTE_FRAME);

    size_t pdpt_idx = PDPT_INDEX(virt);
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
        uint64_t new_table_phys = (uint64_t)vmm_get_phys_page();
        memset(phys_to_virt(new_table_phys), 0, PAGE_SIZE);
        pdpt[pdpt_idx] = new_table_phys | PTE_PRESENT | PTE_WRITABLE | table_user_flag;
    } else {
        if (table_user_flag) pdpt[pdpt_idx] |= PTE_USER;
    }
    page_table_t *pd = phys_to_virt(pdpt[pdpt_idx] & PTE_FRAME);

    size_t pd_idx = PD_INDEX(virt);
    if (!(pd[pd_idx] & PTE_PRESENT)) {
        uint64_t new_table_phys = (uint64_t)vmm_get_phys_page();
        memset(phys_to_virt(new_table_phys), 0, PAGE_SIZE);
        pd[pd_idx] = new_table_phys | PTE_PRESENT | PTE_WRITABLE | table_user_flag;
    } else {
        if (table_user_flag) pd[pd_idx] |= PTE_USER;
    }
    page_table_t *pt = phys_to_virt(pd[pd_idx] & PTE_FRAME);

    size_t pt_idx = PT_INDEX(virt);
    pt[pt_idx] = (phys & PTE_FRAME) | PTE_PRESENT | flags;

    asm volatile("invlpg (%0)" ::"r"(virt) : "memory");
}

static inline page_table_t *vmm_get_next_table(page_table_t *table, uint64_t index) {
    if (!(table[index] & PTE_PRESENT)) {
        return NULL;
    }
    return phys_to_virt(table[index] & PTE_FRAME);
}

static void vmm_unmap_page(page_table_t *pml4, uint64_t virt) {
    page_table_t *pdpt = vmm_get_next_table(pml4, PML4_INDEX(virt));
    if (!pdpt) return;
    page_table_t *pd = vmm_get_next_table(pdpt, PDPT_INDEX(virt));
    if (!pd) return;
    page_table_t *pt = vmm_get_next_table(pd, PD_INDEX(virt));
    if (!pt) return;

    uint64_t entry = pt[PT_INDEX(virt)];
    if (!(entry & PTE_PRESENT)) {
        return;
    }

    uint64_t phys = entry & PTE_FRAME;
    pt[PT_INDEX(virt)] = 0;
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
    pmm_free_pages((void *)(phys + HHDM_OFFSET), 0);
}

static inline bool vmm_page_is_mapped(page_table_t *pml4, uint64_t virt) {
    page_table_t *pdpt = vmm_get_next_table(pml4, PML4_INDEX(virt));
    if (!pdpt) return false;
    page_table_t *pd = vmm_get_next_table(pdpt, PDPT_INDEX(virt));
    if (!pd) return false;
    page_table_t *pt = vmm_get_next_table(pd, PD_INDEX(virt));
    if (!pt) return false;
    return (pt[PT_INDEX(virt)] & PTE_PRESENT) != 0;
}

static bool vmm_region_is_free(page_table_t *pml4, uint64_t start, size_t length) {
    if (length == 0) return true;

    uint64_t end = start + length;
    if (end < start) return false;

    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        if (vmm_page_is_mapped(pml4, addr)) {
            return false;
        }
    }
    return true;
}

static uint64_t vmm_find_free_region(page_table_t *pml4, uint64_t hint, size_t pages) {
    const uint64_t USER_MMAP_LIMIT = 0x00007FFFFFFFF000ULL;
    uint64_t bytes = pages * PAGE_SIZE;
    uint64_t candidate = (hint + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    while (candidate + bytes <= USER_MMAP_LIMIT) {
        if (vmm_region_is_free(pml4, candidate, bytes)) {
            return candidate;
        }
        candidate += PAGE_SIZE;
    }
    return 0;
}

static uint64_t mmap_cursor = 0x00100000ULL;

void *vmm_mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    if (length == 0) return (void *)-1;

    if (!(flags & MAP_ANONYMOUS) || fd != -1 || offset != 0) {
        return (void *)-1;
    }

    size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t target = (uint64_t)addr;
    page_table_t *pml4 = vmm_get_current_pml4();

    if (target == 0) {
        if (mmap_cursor < 0x00100000ULL) {
            mmap_cursor = 0x00100000ULL;
        }
        target = vmm_find_free_region(pml4, mmap_cursor, pages);
        if (target == 0) return (void *)-1;
        mmap_cursor = target + pages * PAGE_SIZE;
    } else {
        if (target & (PAGE_SIZE - 1)) return (void *)-1;
        
        if (flags & MAP_FIXED) {
            if (!vmm_region_is_free(pml4, target, pages * PAGE_SIZE)) {
                return (void *)-1;
            }
        } else {
            if (!vmm_region_is_free(pml4, target, pages * PAGE_SIZE)) {
                target = vmm_find_free_region(pml4, mmap_cursor, pages);
                if (target == 0) return (void *)-1;
                mmap_cursor = target + pages * PAGE_SIZE;
            }
        }
    }

    uint64_t pte_flags = PTE_USER | PTE_PRESENT;
    if (prot & PROT_WRITE) pte_flags |= PTE_WRITABLE;
    if (!(prot & PROT_EXEC)) pte_flags |= PTE_NO_EXECUTE;

    for (size_t i = 0; i < pages; i++) {
        void *page = pmm_alloc_pages(0);
        if (!page) {
            for (size_t j = 0; j < i; j++) {
                vmm_unmap_page(pml4, target + j * PAGE_SIZE);
            }
            return (void *)-1;
        }
        memset(page, 0, PAGE_SIZE);
        uint64_t phys = (uint64_t)page - HHDM_OFFSET;
        vmm_map_page(pml4, target + i * PAGE_SIZE, phys, pte_flags);
    }

    return (void *)target;
}

int vmm_munmap(void *addr, size_t length) {
    if (addr == NULL || length == 0) return -1;

    uint64_t start = (uint64_t)addr & ~(PAGE_SIZE - 1);
    uint64_t end = ((uint64_t)addr + length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    page_table_t *pml4 = vmm_get_current_pml4();

    for (uint64_t current = start; current < end; current += PAGE_SIZE) {
        page_table_t *pdpt = vmm_get_next_table(pml4, PML4_INDEX(current));
        if (!pdpt) continue;
        page_table_t *pd = vmm_get_next_table(pdpt, PDPT_INDEX(current));
        if (!pd) continue;
        page_table_t *pt = vmm_get_next_table(pd, PD_INDEX(current));
        if (!pt) continue;

        uint64_t entry = pt[PT_INDEX(current)];
        if (!(entry & PTE_PRESENT)) continue;

        uint64_t phys = entry & PTE_FRAME;
        pt[PT_INDEX(current)] = 0;
        asm volatile("invlpg (%0)" :: "r"(current) : "memory");
        pmm_free_pages((void *)(phys + HHDM_OFFSET), 0);
    }
    return 0;
}