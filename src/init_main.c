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
#define ARENA_SIZE (1024 * 1024 * 16) // 16 MB pool
static uint8_t elf_alloc_arena[ARENA_SIZE];
static uint64_t arena_offset = 0;

// A simple, bulletproof allocation bypass
void* arena_alloc(uint64_t size) {
    // Align allocations to 16 bytes for hardware safety
    uint64_t aligned_size = (size + 15) & ~15;

    if (arena_offset + aligned_size > ARENA_SIZE) {
        return 0; // Out of memory
    }

    void* ptr = &elf_alloc_arena[arena_offset];
    arena_offset += aligned_size;
    return ptr;
}

// Optional: Reset the pointer when you are completely done with the task
void arena_reset(void) {
    arena_offset = 0;
}
static void main_kthread(void) {
    printk(LOG_TRACE, "main kthread started.\n");
    char hi[3] = "hi";
    int fd_test = vfs_create_file(hi, "main.txt", 3);
    printk(LOG_DEBUG, "Test File FD (main.py): %d\n", fd_test);

    // Create an isolated, sandboxed user address space context.
    page_table_t *user_pml4 = vmm_create_address_space();
    if (user_pml4 == NULL) {
        printk(LOG_ERROR, "Could not allocate isolated user address space\n");
        for(;;);
    }
    
    // User virtual memory space layouts matching your linker script
    uint64_t user_code_vma  = 0x2000000; 
    uint64_t user_stack_vma = 0x600000;

    // Open the user ELF binary file
    int user_fd = vfs_open("user_app.elf");
    if (user_fd < 0) {
        printk(LOG_ERROR, "Could not open user_app.elf\n");
        for(;;);
    }

    uint64_t user_flags = PTE_USER | PTE_WRITABLE;
    
    // Physical bases configuration
    uint64_t safe_code_phys_base  = 0x8000000;  
    uint64_t safe_stack_phys_base = 0xA200000;  

    // Staging buffer math: 0x8000000 + (9216 * 4096) = 0xA400000
    uint64_t raw_elf_phys_base    = safe_code_phys_base + (9216 * PAGE_SIZE); 
    void* raw_elf_hhdm_ptr        = (void*)(raw_elf_phys_base + HHDM_OFFSET);

    printk(LOG_DEBUG, "[VERBOSE] safe_code_phys_base:  0x%llx\n", safe_code_phys_base);
    printk(LOG_DEBUG, "[VERBOSE] raw_elf_phys_base:     0x%llx\n", raw_elf_phys_base);
    printk(LOG_DEBUG, "[VERBOSE] raw_elf_hhdm_ptr:      0x%llx\n", (uint64_t)raw_elf_hhdm_ptr);

    int file_cursor = 0;
    uint64_t total_bytes_read = 0;
    int page_index = 0;
    int first_chunk = 1;

    // ============================================================================
    // STEP 1: Read raw ELF file sequentially into staging buffer + VERBOSE LOGS
    // ============================================================================
    while (1) {
        void *current_dest_ptr = (void *)((uint8_t*)raw_elf_hhdm_ptr + total_bytes_read);
        
        int chunks_read = vfs_read(user_fd, current_dest_ptr, PAGE_SIZE, file_cursor);
        
        if (first_chunk) {
            printk(LOG_DEBUG, "[VERBOSE] First VFS read returned: %d bytes\n", chunks_read);
            if (chunks_read > 0) {
                uint8_t *b = (uint8_t *)current_dest_ptr;
                printk(LOG_DEBUG, "[VERBOSE] First 16 bytes in staging buffer:\n");
                printk(LOG_DEBUG, "          %02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x\n",
                       b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                       b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
            }
            first_chunk = 0;
        }

        if (chunks_read <= 0) {
            break; 
        }

        total_bytes_read += chunks_read;
        file_cursor += chunks_read;
        page_index++;
    }
    
    uint8_t *verify_bytes = (uint8_t*)(safe_code_phys_base + HHDM_OFFSET);
    printk(LOG_DEBUG, "Bytes at physical base destination: %02x %02x %02x %02x\n", 
       verify_bytes[0], verify_bytes[1], verify_bytes[2], verify_bytes[3]);
    printk(LOG_TRACE, "Cached raw ELF from VFS: %llu bytes read into temporary staging.\n", total_bytes_read);
    vfs_free_fd(user_fd);

    if (total_bytes_read <= 0) {
        printk(LOG_ERROR, "user_app.elf is empty or failed to read!\n");
        for(;;);
    }
    
    verify_bytes = (uint8_t*)(safe_code_phys_base + HHDM_OFFSET);
    printk(LOG_DEBUG, "Bytes at physical base destination: %02x %02x %02x %02x\n", 
       verify_bytes[0], verify_bytes[1], verify_bytes[2], verify_bytes[3]);

    // ============================================================================
    // STEP 2: Map execution page memory window up-front 
    // ============================================================================
    int staging_pages = 8300; 
    printk(LOG_DEBUG, "[VERBOSE] Mapping %d pages starting at VMA 0x%llx\n", staging_pages, user_code_vma);
    for (int i = 0; i < staging_pages; i++) {
        uint64_t current_phys = safe_code_phys_base + (i * PAGE_SIZE);
        uint64_t current_vma  = user_code_vma + (i * PAGE_SIZE);
        
        memset((void*)(current_phys + HHDM_OFFSET), 0, PAGE_SIZE);
        vmm_map_page(user_pml4, current_vma, current_phys, user_flags);
    }
    
    verify_bytes = (uint8_t*)(safe_code_phys_base + HHDM_OFFSET);
    printk(LOG_DEBUG, "Bytes at physical base destination: %02x %02x %02x %02x\n", 
       verify_bytes[0], verify_bytes[1], verify_bytes[2], verify_bytes[3]);

    // ============================================================================
    // STEP 3: Run the HHDM-secured ELF loader (FIXED BASE ARGUMENTS)
    // ============================================================================
    printk(LOG_DEBUG, "[VERBOSE] Invoking load_elf with staging ptr...\n");
    // CRITICAL FIX: Explicitly passing target physical destination and virtual base offsets
    ElfLoadResult loaded_app = load_elf(raw_elf_hhdm_ptr, safe_code_phys_base, user_code_vma);

    if (loaded_app.entry_point == 0) {
        printk(LOG_ERROR, "ELF Loader failed to validate or parse user_app.elf!\n");
        for(;;);
    }
    
    verify_bytes = (uint8_t*)(safe_code_phys_base + HHDM_OFFSET);
    printk(LOG_DEBUG, "Bytes at physical base destination: %02x %02x %02x %02x\n", 
       verify_bytes[0], verify_bytes[1], verify_bytes[2], verify_bytes[3]);

    uint64_t program_pages_used = (loaded_app.mem_size + PAGE_SIZE - 1) / PAGE_SIZE;
    printk(LOG_DEBUG, "[VERBOSE] load_elf succeeded. mem_size pages count: %llu\n", program_pages_used);

    // Expand program mappings if size exceeded initial guest bounds
    if (program_pages_used > (uint64_t)staging_pages) {
        printk(LOG_DEBUG, "[VERBOSE] Expanding mappings by %llu pages\n", program_pages_used - staging_pages);
        for (uint64_t i = staging_pages; i < program_pages_used; i++) {
            uint64_t current_phys = safe_code_phys_base + (i * PAGE_SIZE);
            uint64_t current_vma  = user_code_vma + (i * PAGE_SIZE);
            memset((void*)(current_phys + HHDM_OFFSET), 0, PAGE_SIZE);
            vmm_map_page(user_pml4, current_vma, current_phys, user_flags);
        }
    }

    // ============================================================================
    // STEP 4: Setup Heap Space (.sbrk) matching your linker script _heap_start
    // ============================================================================
    const int user_heap_pages = 8192*2; 
    uint64_t user_heap_vma = user_code_vma + (program_pages_used * PAGE_SIZE);
    printk(LOG_DEBUG, "[VERBOSE] Mapping Heap starting at VMA 0x%llx\n", user_heap_vma);
    
    for (int heap_page = 0; heap_page < user_heap_pages; heap_page++) {
        uint64_t heap_phys = safe_code_phys_base + ((program_pages_used + heap_page) * PAGE_SIZE);
        void *heap_hhdm_ptr = (void *)(heap_phys + HHDM_OFFSET);
        memset(heap_hhdm_ptr, 0, PAGE_SIZE);
        vmm_map_page(user_pml4, user_heap_vma + ((uint64_t)heap_page * PAGE_SIZE), heap_phys, user_flags);
    }
    
    verify_bytes = (uint8_t*)(safe_code_phys_base + HHDM_OFFSET);
    printk(LOG_DEBUG, "Bytes at physical base destination: %02x %02x %02x %02x\n", 
       verify_bytes[0], verify_bytes[1], verify_bytes[2], verify_bytes[3]);
       
    // ============================================================================
    // STEP 5: Map a Multi-Page Stack Region and Calculate the Initial RSP
    // ============================================================================
    int user_stack_pages = 64; 
    printk(LOG_DEBUG, "[VERBOSE] Mapping Stack starting at VMA 0x%llx\n", user_stack_vma);
    
    for (int i = 0; i < user_stack_pages; i++) {
        uint64_t stack_phys = safe_stack_phys_base + (i * PAGE_SIZE);
        void *stack_hhdm_ptr = (void *)(stack_phys + HHDM_OFFSET);
        memset(stack_hhdm_ptr, 0, PAGE_SIZE);
        vmm_map_page(user_pml4, user_stack_vma + (i * PAGE_SIZE), stack_phys, user_flags);
    }

    uint64_t initial_rsp = user_stack_vma + (user_stack_pages * PAGE_SIZE) - 16;

    printk(LOG_TRACE, "ELF Configured: Entry=0x%llx, Program Size=%llu bytes, Stack Top RSP=0x%llx\n", 
           loaded_app.entry_point, loaded_app.mem_size, initial_rsp);
           
    verify_bytes = (uint8_t*)(safe_code_phys_base + HHDM_OFFSET);
    printk(LOG_DEBUG, "Bytes at physical base destination: %02x %02x %02x %02x\n", 
       verify_bytes[0], verify_bytes[1], verify_bytes[2], verify_bytes[3]);
    flanterm_clear(ft_ctx, true);
    int pid = create_user_task((void *)loaded_app.entry_point, (void *)initial_rsp);
    
    packet data;
    data.r = 255;
    data.g = 0;
    data.b = 255;
    data.rect_width = 1920;
    data.rect_height = 1080;
    data.rect_x = 0;
    data.rect_y = 0;
    ipc_send(pid, &data, sizeof(data));

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
    fat32_init(get_volume(0), &efi);
    if (create_kernel_task(main_kthread) < 0) {
        printk(LOG_ERROR, "Failed to create main kthread.\n");
        hlt();
    }
    start_scheduler();
}
