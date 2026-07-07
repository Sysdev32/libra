// SPDX-License-Identifier: GPL-3.0-only

#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include <vendor/flanterm/flanterm.h>
#include <vendor/flanterm/flanterm_backends/fb.h>
#include <drivers/fb.h>
#include <drivers/gdt.h>
#include <drivers/alloc.h>
#include <drivers/idt.h>
#include <drivers/schedule.h>
#include <drivers/vfs.h>
#include <drivers/pci.h>
#include <drivers/gpt.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <uacpi/event.h>
#include <string.h>
#include <uacpi/tables.h>
#include <config.h>
#include <drivers/fat32.h>
#include <state.h>
#include <uacpi/resources.h>
#include <uacpi/internal/namespace.h>
#include <uacpi/internal/stdlib.h>
#include <uacpi/types.h>
#include <errno.h>
#include <drivers/elf.h>
struct flanterm_context *ft_ctx;
// Forward declarations for VMM helpers (defined in drivers/helpalloc.c)
typedef uint64_t page_table_t;
page_table_t *vmm_create_address_space(void);
void vmm_map_page(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
extern volatile struct limine_memmap_request memmap_request;
// 1. Correct Start Marker Setup
__attribute__((used, section(".limine_requests_start")))
volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

// 2. Your standard feature requests go here
__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

// 3. Correct End Marker Setup
__attribute__((used, section(".limine_requests_end")))
volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

void hlt(void) {
    for (;;) {
        asm("hlt");
    }
}

static void init_sse(void) {
    unsigned long cr0, cr4;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    // Clear EM (bit 2) to enable x87/MMX/SSE instructions
    cr0 &= ~(1UL << 2);
    // Set MP (bit 1) so FPU behaves correctly with WAIT/FWAIT
    cr0 |= (1UL << 1);
    asm volatile("mov %0, %%cr0" :: "r"(cr0));

    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    // Enable OSFXSR (bit 9) and OSXMMEXCPT (bit 10)
    cr4 |= (1UL << 9) | (1UL << 10);
    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    // Load a sane MXCSR default (all exceptions masked)
    unsigned int mxcsr = 0x1f80;
    asm volatile("ldmxcsr %0" :: "m"(mxcsr));
}

void pit_init(void) {
    // 1193182 Hz is the internal oscillator speed of the PIT chip
    uint32_t frequency = 100; // 100 Hz = 10ms interval
    uint16_t divisor = 1193182 / frequency;

    // Command port 0x43: 
    // 0x36 = Binary counter, Mode 3 (Square Wave), Load LSB then MSB, Channel 0
    outb(0x43, 0x36);

    // Data port 0x40: Send the split 16-bit divisor value
    outb(0x40, (uint8_t)(divisor & 0xFF));        // Low byte (LSB)
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF)); // High byte (MSB)
    
    printk(LOG_TRACE, "PIT Timer initialized at 100Hz.\n");
}
__attribute__((section(".user_text"))) void uthread(void) {
    asm volatile(
        "1:\n\t"       // Define local label 1
        "jmp 1b\n\t"   // Jump backward ('b') to label 1
        :
        :
        : "memory"
    );
}


extern char __user_src_start[];
extern char __user_src_end[];
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_FRAME    0x000FFFFFFFFFF000ULL 
#define HHDM_OFFSET  0xffff800000000000ULL
#define PAGE_SIZE 4096
typedef uint64_t page_table_t;
static void ps2_wait_input(void)
{
    while (inb(0x64) & 2);
}

static void ps2_wait_output(void)
{
    while (!(inb(0x64) & 1));
}
void keyboard_init(void)
{
    uint8_t config;

    ps2_wait_input();
    outb(0x64, 0xAD); // disable port 1

    ps2_wait_input();
    outb(0x64, 0x20); // read config byte

    ps2_wait_output();
    config = inb(0x60);

    config |= 1;   // enable interrupt on keyboard

    ps2_wait_input();
    outb(0x64, 0x60);
    ps2_wait_input();
    outb(0x60, config);

    ps2_wait_input();
    outb(0x64, 0xAE); // enable keyboard

    // optional: clear buffer
    inb(0x60);
}
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    int rect_x;
    int rect_y;
    int rect_width;
    int rect_height;
} packet;
#include <stdint.h>
#include <stddef.h>

#define ARENA_START 0x6000000ULL
#define ARENA_END   0x8000000ULL
#define PAGE_SIZE   0x1000ULL

#define NUM_PAGES ((ARENA_END - ARENA_START) / PAGE_SIZE)
#define MAX_ALLOCS 128

static uint8_t bitmap[NUM_PAGES / 8];

struct arena_alloc {
    void *addr;
    size_t pages;
    int used;
};

static struct arena_alloc allocs[MAX_ALLOCS];

static inline int test_page(size_t i) {
    return (bitmap[i / 8] >> (i % 8)) & 1;
}

static inline void set_page(size_t i) {
    bitmap[i / 8] |= (1 << (i % 8));
}

static inline void clear_page(size_t i) {
    bitmap[i / 8] &= ~(1 << (i % 8));
}

void *arena_alloc(size_t bytes) {
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    size_t run = 0, start = 0;

    for (size_t i = 0; i < NUM_PAGES; i++) {
        if (!test_page(i)) {
            if (run == 0)
                start = i;

            if (++run == pages) {
                for (size_t j = start; j < start + pages; j++)
                    set_page(j);

                void *addr = (void *)(ARENA_START + start * PAGE_SIZE);

                for (size_t k = 0; k < MAX_ALLOCS; k++) {
                    if (!allocs[k].used) {
                        allocs[k].used = 1;
                        allocs[k].addr = addr;
                        allocs[k].pages = pages;
                        break;
                    }
                }

                return addr;
            }
        } else {
            run = 0;
        }
    }

    return NULL;
}

void arena_free(void *addr) {
    for (size_t i = 0; i < MAX_ALLOCS; i++) {
        if (allocs[i].used && allocs[i].addr == addr) {
            size_t start = ((uintptr_t)addr - ARENA_START) / PAGE_SIZE;

            for (size_t j = 0; j < allocs[i].pages; j++)
                clear_page(start + j);

            allocs[i].used = 0;
            return;
        }
    }
}
#include <stdint.h>
#include <stddef.h>

#define STACK_PHYS_START 0x0A200000ULL
#define STACK_PHYS_END   0x0C200000ULL   // 32 MiB for stacks

#define PAGE_SIZE   0x1000ULL
#define NUM_PAGES   ((STACK_PHYS_END - STACK_PHYS_START) / PAGE_SIZE)
#define MAX_STACKS  128

static uint8_t stack_bitmap[NUM_PAGES / 8];

struct stack_alloc {
    uint64_t phys;
    size_t pages;
    int used;
};

static struct stack_alloc stacks[MAX_STACKS];

static inline int stack_test_page(size_t i) {
    return (stack_bitmap[i / 8] >> (i % 8)) & 1;
}

static inline void stack_set_page(size_t i) {
    stack_bitmap[i / 8] |= (1 << (i % 8));
}

static inline void stack_clear_page(size_t i) {
    stack_bitmap[i / 8] &= ~(1 << (i % 8));
}

uint64_t stack_alloc(size_t bytes) {
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    size_t run = 0, start = 0;

    for (size_t i = 0; i < NUM_PAGES; i++) {
        if (!stack_test_page(i)) {
            if (run == 0)
                start = i;

            if (++run == pages) {
                for (size_t j = start; j < start + pages; j++)
                    stack_set_page(j);

                uint64_t phys = STACK_PHYS_START + start * PAGE_SIZE;

                for (size_t k = 0; k < MAX_STACKS; k++) {
                    if (!stacks[k].used) {
                        stacks[k].used = 1;
                        stacks[k].phys = phys;
                        stacks[k].pages = pages;
                        break;
                    }
                }

                return phys;
            }
        } else {
            run = 0;
        }
    }

    return 0;
}

void stack_free(uint64_t phys) {
    for (size_t i = 0; i < MAX_STACKS; i++) {
        if (stacks[i].used && stacks[i].phys == phys) {
            size_t start = (phys - STACK_PHYS_START) / PAGE_SIZE;

            for (size_t j = 0; j < stacks[i].pages; j++)
                stack_clear_page(start + j);

            stacks[i].used = 0;
            return;
        }
    }
}
#define HEAP_START ((uint64_t)0x40000000)
#define HEAP_END   ((uint64_t)0x60000000)
int spawn(char* path) {
    printk(LOG_TRACE, "[SPAWN] Entering spawn() with user path pointer: %p\n", (void*)path);

    if (!path) {
        printk(LOG_ERROR, "[SPAWN] Error: User-supplied path pointer is NULL!\n");
        return -1;
    }

    // Safely copy the user-supplied path into kernel memory BEFORE
    // creating a new address space (which will switch CR3).
    char kpath[256];
    printk(LOG_TRACE, "[SPAWN] Copying user path string safely to kernel buffer...\n");
    
    for (int i = 0; i < 255; i++) {
        char c = path[i];
        kpath[i] = c;
        if (c == '\0') {
            printk(LOG_TRACE, "[SPAWN] Null-terminator found at index %d. Path string: \"%s\"\n", i, kpath);
            break;
        }
        if (i == 254) {
            kpath[255] = '\0';
            printk(LOG_WARNING, "[SPAWN] Warning: Path string exceeded 254 characters! Truncating.\n");
        }
    }

    printk(LOG_TRACE, "[SPAWN] Requesting creation of an isolated user address space (PML4)...\n");
    page_table_t *user_pml4 = vmm_create_address_space();
    
    if (user_pml4 == NULL) {
        printk(LOG_ERROR, "[SPAWN] CRITICAL ERROR: Could not allocate isolated user address space (PML4 is NULL)!\n");
        for(;;);
    }
    printk(LOG_TRACE, "[SPAWN] Successfully allocated isolated user address space. PML4 CR3 Root: %p\n", (void*)user_pml4);

    printk(LOG_TRACE, "[SPAWN] Attempting VFS open on kernel-safe path: \"%s\"\n", kpath);
    int user_fd = vfs_open(kpath);
    
    if (user_fd < 0) {
        printk(LOG_ERROR, "[SPAWN] CRITICAL ERROR: Could not open file \"%s\". VFS Error Code: %d\n", kpath, user_fd);
        for(;;);
    }
    printk(LOG_TRACE, "[SPAWN] File successfully opened. Assigned File Descriptor (FD): %d\n", user_fd);

    uint64_t user_flags = PTE_USER | PTE_WRITABLE;
    struct vfs_stat stat = {0};
    
    printk(LOG_TRACE, "[SPAWN] Fetching file stats via vfs_fstat for FD %d...\n", user_fd);
    vfs_fstat(user_fd, &stat);
    printk(LOG_TRACE, "[SPAWN] File Size Metrics: %zu bytes (Blocks allocated: %zu)\n", (size_t)stat.st_size, (size_t)stat.st_blocks);

    // Physical bases configuration
    printk(LOG_TRACE, "[SPAWN] Allocating physical arenas...\n");
    uint64_t safe_code_phys_base  = arena_alloc(stat.st_size);  
    printk(LOG_TRACE, "[SPAWN] -> safe_code_phys_base allocated: %p (Size: %zu bytes)\n", (void*)safe_code_phys_base, (size_t)stat.st_size);

    uint64_t safe_stack_phys_base = stack_alloc(256 * 1024);  
    printk(LOG_TRACE, "[SPAWN] -> safe_stack_phys_base allocated: %p (Size: 256 KB)\n", (void*)safe_stack_phys_base);

    // Staging buffer math: 0x8000000 + (9216 * 4096) = 0xA400000
    uint64_t raw_elf_phys_base    = safe_code_phys_base + (9216 * PAGE_SIZE); 
    void* raw_elf_hhdm_ptr        = (void*)(raw_elf_phys_base + HHDM_OFFSET);
    
    printk(LOG_TRACE, "[SPAWN] Staging Buffer Offset Math:\n");
    printk(LOG_TRACE, "        -> raw_elf_phys_base: %p\n", (void*)raw_elf_phys_base);
    printk(LOG_TRACE, "        -> raw_elf_hhdm_ptr:  %p (HHDM Offset applied)\n", raw_elf_hhdm_ptr);

    int file_cursor = 0;
    uint64_t total_bytes_read = 0;
    int page_index = 0;
    int first_chunk = 1;

    // ============================================================================
    // STEP 1: Read raw ELF file sequentially into staging buffer + VERBOSE LOGS
    // ============================================================================
    printk(LOG_TRACE, "[SPAWN] Starting raw ELF file streaming sequential read loop...\n");
    while (1) {
        void *current_dest_ptr = (void *)((uint8_t*)raw_elf_hhdm_ptr + total_bytes_read);
        
        printk(LOG_TRACE, "[SPAWN] Reading Page %d -> Requesting %d bytes from File Cursor %d to target RAM ptr: %p\n", 
               page_index, PAGE_SIZE, file_cursor, current_dest_ptr);
        
        int chunks_read = vfs_read(user_fd, current_dest_ptr, PAGE_SIZE, file_cursor);
        
        if (first_chunk) {
            printk(LOG_TRACE, "[SPAWN] First chunk read result: %d bytes. Checking ELF Magic Header...\n", chunks_read);
            uint8_t *magic = (uint8_t*)current_dest_ptr;
            printk(LOG_TRACE, "[SPAWN] Magic bytes: 0x%02x 0x%02x 0x%02x 0x%02x\n", magic[0], magic[1], magic[2], magic[3]);
            first_chunk = 0;
        }

        if (chunks_read <= 0) {
            printk(LOG_TRACE, "[SPAWN] Stream loop terminated. vfs_read returned: %d (EOF or Error context)\n", chunks_read);
            break; 
        }

        total_bytes_read += chunks_read;
        file_cursor += chunks_read;
        page_index++;
    }
    
    printk(LOG_TRACE, "[SPAWN] Read loop finished. Total cumulative bytes streamed into RAM: %zu bytes across %d chunks.\n", (size_t)total_bytes_read, page_index);

    printk(LOG_TRACE, "[SPAWN] Parsing ELF virtual target entry address from raw memory pointer %p...\n", raw_elf_hhdm_ptr);
    uint64_t user_code_vma  = elf_vaddr(raw_elf_hhdm_ptr); 
    uint64_t user_stack_vma = 0x600000;
    
    printk(LOG_TRACE, "[SPAWN] Parsed Layout Context:\n");
    printk(LOG_TRACE, "        -> Target User Code VMA Base: %p\n", (void*)user_code_vma);
    printk(LOG_TRACE, "        -> Target User Stack VMA Base: %p\n", (void*)user_stack_vma);

    printk(LOG_TRACE, "[SPAWN] Closing VFS File Descriptor %d and cleaning resources...\n", user_fd);
    vfs_free_fd(user_fd);

    if (total_bytes_read <= 0) {
        printk(LOG_ERROR, "[SPAWN] CRITICAL ERROR: user_app.elf read verification failed! Total bytes read is 0 or negative.\n");
        for(;;);
    }

    // ============================================================================
    // STEP 2: Map execution page memory window up-front 
    // ============================================================================
    int staging_pages = 8300; 
    printk(LOG_TRACE, "[SPAWN] Beginning execution page table allocation window up-front mapping sequence (%d pages)...\n", staging_pages);
    
    for (int i = 0; i < staging_pages; i++) {
        uint64_t current_phys = safe_code_phys_base + (i * PAGE_SIZE);
        uint64_t current_vma  = user_code_vma + (i * PAGE_SIZE);
        void *clear_ptr = (void*)(current_phys + HHDM_OFFSET);
        
        // Log every 1000 pages to prevent output log drowning, but keep it trace-heavy
        if (i == 0 || i == staging_pages - 1 || i % 1000 == 0) {
            printk(LOG_TRACE, "[SPAWN] Mapping Execution Frame [%d/%d]: Phys %p -> VMA %p (Zeroing HHDM: %p)\n", 
                   i, staging_pages - 1, (void*)current_phys, (void*)current_vma, clear_ptr);
        }
        
        memset(clear_ptr, 0, PAGE_SIZE);
        vmm_map_page(user_pml4, current_vma, current_phys, user_flags);
    }
    printk(LOG_TRACE, "[SPAWN] Execution page table window fully pre-mapped into PML4 root structural context.\n");

    // CRITICAL FIX: Explicitly passing target physical destination and virtual base offsets
    printk(LOG_TRACE, "[SPAWN] Passing payload to ELF Dynamic Program Loader...\n");
    printk(LOG_TRACE, "        Args: Raw HHDM Ptr: %p, Dest Phys Base: %p, Target VMA Base: %p\n", 
           raw_elf_hhdm_ptr, (void*)safe_code_phys_base, (void*)user_code_vma);
    
    ElfLoadResult loaded_app = load_elf(raw_elf_hhdm_ptr, safe_code_phys_base, user_code_vma);

    printk(LOG_TRACE, "[SPAWN] ELF Loader execution phase concluded.\n");
    printk(LOG_TRACE, "        -> Entry Point Result: %p\n", (void*)loaded_app.entry_point);

    if (loaded_app.entry_point == 0) {
        printk(LOG_ERROR, "[SPAWN] CRITICAL ERROR: ELF Loader failed to validate, parse, or resolve segments of the target ELF!\n");
        for(;;);
    }

    // The user heap will grow lazily via sbrk/mmap from userland; do not pre-map the
    // entire reserved heap region. This avoids a 512MB eager allocation at boot.
    // ============================================================================
    // STEP 5: Map a Multi-Page Stack Region and Calculate the Initial RSP
    // ============================================================================
    int user_stack_pages = 64; 
    printk(LOG_TRACE, "[SPAWN] Setting up stack context. Mapping user land runtime stack region (%d pages total)...\n", user_stack_pages);
    
    for (int i = 0; i < user_stack_pages; i++) {
        uint64_t stack_phys = safe_stack_phys_base + (i * PAGE_SIZE);
        uint64_t stack_vma  = user_stack_vma + (i * PAGE_SIZE);
        void *stack_hhdm_ptr = (void *)(stack_phys + HHDM_OFFSET);
        
        if (i == 0 || i == user_stack_pages - 1 || i % 16 == 0) {
            printk(LOG_TRACE, "[SPAWN] Mapping Stack Frame [%d/%d]: Phys %p -> VMA %p (Zeroing HHDM: %p)\n", 
                   i, user_stack_pages - 1, (void*)stack_phys, (void*)stack_vma, stack_hhdm_ptr);
        }
        
        memset(stack_hhdm_ptr, 0, PAGE_SIZE);
        vmm_map_page(user_pml4, stack_vma, stack_phys, user_flags);
    }

    uint64_t initial_rsp = user_stack_vma + (user_stack_pages * PAGE_SIZE) - 16;
    printk(LOG_TRACE, "[SPAWN] Calculated Initial Stack Pointer (RSP) location: %p (16-byte alignment applied)\n", (void*)initial_rsp);

    printk(LOG_TRACE, "[SPAWN] Ultimate verification check before executing task switch:\n");
    printk(LOG_TRACE, "        -> Entry point RIP Target:     %p\n", (void*)loaded_app.entry_point);
    printk(LOG_TRACE, "        -> Target Initial RSP Context: %p\n", (void*)initial_rsp);
    printk(LOG_TRACE, "        -> Page Table Root CR3 Context:%p\n", (void*)user_pml4);
    
    printk(LOG_TRACE, "[SPAWN] Spawning thread now. Yielding context to scheduler/task creator...\n");
    return create_user_task((void *)loaded_app.entry_point, (void *)initial_rsp, user_pml4, 0, 0);
}
static void main_kthread(void) {
    printk(LOG_TRACE, "MAIN KTHREAD HAS ARRIVED LOL OLLKOLLOL!!!!!\n");
    spawn("/System/usr/bin/graphical/WindowManager");
    for (;;) {
        asm volatile("sti; hlt");
    }
}
int dump_namespace(uacpi_namespace_node *root)
{
    if (!root)
        return -EINVAL;

    uacpi_namespace_node *node = root;

    while (node) {

        int depth = 0;
        uacpi_namespace_node *tmp = node->parent;

        while (tmp) {
            depth++;
            tmp = tmp->parent;
        }

        for (int i = 0; i < depth; i++)
            printk(LOG_NONE, "  ");

        printk(LOG_NONE, "%c%c%c%c\n",
               node->name.text[0],
               node->name.text[1],
               node->name.text[2],
               node->name.text[3]);

        if (node->child) {
            node = node->child;
            continue;
        }

        while (node) {
            if (node->next) {
                node = node->next;
                break;
            }

            node = node->parent;
        }
    }
}
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void triple_fault_reboot(void) {
    // Create an IDT pointer with a limit of 0
    struct idt_ptr invalid_idt;
    invalid_idt.limit = 0;
    invalid_idt.base = 0;

    // Load the invalid IDT into the CPU
    asm volatile("lidt %0" :: "m"(invalid_idt));

    // Step 2: Trigger an interrupt to force the crash
    asm volatile("int $3");

    // Hang just in case the CPU takes a moment to reset
    for (;;);
}

/* SSE initialization removed: we do not enable OSFXSR/OSXMMEXCPT or touch MXCSR here. */
// Your Kernel Entry Point
pci_device_t* devices;
uint32_t devicecount = 0;
void _start(void) {
    // Ensure the bootloader answered our framebuffer request safely
    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        hlt();
    }
    
    gdt_init();
    idt_init();
    uint64_t total_usable_memory = 0;
    uint64_t entries_count = memmap_request.response->entry_count;

    for (size_t i = 0; i < entries_count; i++) {
        struct limine_memmap_entry *entry = memmap_request.response->entries[i];

        // Check if this specific region of RAM is free to use
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            total_usable_memory += entry->length;
        }
    }
    
    // FIX 1: Access the array base element pointer directly
    struct limine_framebuffer **framebuffers = framebuffer_request.response->framebuffers;
    struct limine_framebuffer *framebuffer = framebuffers[0];

    // FIX 2: Explicitly cast void* address to a 32-bit unsigned integer pointer
    uint32_t *fb_ptr = (uint32_t *)framebuffer->address;

    ft_ctx = flanterm_fb_init(
        NULL,
        NULL,
        fb_ptr, framebuffer->width, framebuffer->height, framebuffer->pitch,
        framebuffer->red_mask_size, framebuffer->red_mask_shift,
        framebuffer->green_mask_size, framebuffer->green_mask_shift,
        framebuffer->blue_mask_size, framebuffer->blue_mask_shift,
        NULL,
        NULL, NULL,
        NULL, NULL,
        NULL, NULL,
        NULL, 0, 0, 1,
        1.5, 1.5,
        0,
        0
    );
    memory_init();
    flanterm_set_text_fg(ft_ctx, 7, true);
    flanterm_write(ft_ctx, "\033[?25l", 6);
    initConsole(ft_ctx, framebuffer);
    init_vfs();
    
    if (total_usable_memory / 1024 / 1024 < 128) {
        printk(LOG_ERROR, "Less than 128 MB of usable memory detected. Rebooting now..\n");
        triple_fault_reboot();
    }
    printk(LOG_TRACE, "Total usable memory: %d MB\n", total_usable_memory / 1024 / 1024);
    uacpi_status ret = uacpi_initialize(0);
    if (uacpi_unlikely_error(ret)) {
        printk(LOG_ERROR, "uacpi_initialize: %s", uacpi_status_to_string(ret));
    }
    struct acpi_table_madt *madt = NULL;
    ret = uacpi_table_find_by_signature("APIC", (struct acpi_table**)&madt);
    if (ret == UACPI_STATUS_OK) {
        printk(LOG_TRACE, "Found MADT at %p\n", madt);
    } else {
        printk(LOG_ERROR, "MADT not found: %s\n", uacpi_status_to_string(ret));
    }
    ioapic(madt);

    ret = uacpi_namespace_load();
    if (uacpi_unlikely_error(ret)) {
        printk(LOG_ERROR, "uacpi_namespace_load: %s", uacpi_status_to_string(ret));
        for(;;);
    }
    init_sse();
    init_scheduler();

    /*
     * Initialize the namespace. This calls all necessary _STA/_INI AML methods,
     * as well as _REG for registered operation region handlers.
     */
    ret = uacpi_namespace_initialize();
    if (uacpi_unlikely_error(ret)) {
        printk(LOG_ERROR, "uacpi_namespace_initialize: %s", uacpi_status_to_string(ret));
        for(;;);
    }
    
    /*
     * Tell the firmware the interrupt model we're planning to use.
     * (Use UACPI_INTERRUPT_MODEL_PIC if you're planning to use PIC, or any
     *  other value depending on the architecture).
     */
    uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);

    /*
     * Tell uACPI that we have marked all GPEs we wanted for wake (even though we haven't
     * actually marked any, as we have no power management support right now). This is
     * needed to let uACPI enable all unmarked GPEs that have a corresponding AML handler.
     * These handlers are used by the firmware to dynamically execute AML code at runtime
     * to e.g. react to thermal events or device hotplug.
     */
    ret = uacpi_finalize_gpe_initialization();
    if (uacpi_unlikely_error(ret)) {
        printk(LOG_ERROR, "uACPI GPE initialization: %s", uacpi_status_to_string(ret));
        for(;;);
    }
    asm volatile ("sti");
    keyboard_init();
    devices = kcalloc(256, sizeof(pci_device_t));
    
    pci_scan_bus(devices, 256, &devicecount);
    for (int i=0; i<devicecount; i++) {
        printk(LOG_INFO, "PCI DEVICE: %d:%d:%d %x:%x %x:%x\n", devices[i].bus, devices[i].device, devices[i].function, devices[i].class_code, devices[i].subclass, devices[i].device_id, devices[i].vendor_id);
    }
    init_ahci();
    gpt_parse_partitions(get_primary_sata_drive());
    fat32_fs_t efi;
    init_rtl8139();
    discover();
    create_kernel_task(main_kthread);
    start_scheduler();
}
