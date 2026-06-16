// SPDX-License-Identifier: GPL-3.0-only

#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include <vendor/flanterm/flanterm.h>
#include <vendor/flanterm/flanterm_backends/fb.h>
#include <drivers/fb.h>
#include <drivers/gdt.h>
#include <drivers/alloc.h>
#include <drivers/idt.h>
#include <drivers/schedule.h>
#include <drivers/vfs.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <uacpi/event.h>
#include <string.h>
#include <uacpi/tables.h>
#include <config.h>
#include <state.h>
#include <uacpi/resources.h>
#include <uacpi/internal/namespace.h>
#include <uacpi/internal/stdlib.h>
#include <uacpi/types.h>
// Forward declarations for VMM helpers (defined in drivers/helpalloc.c)
typedef uint64_t page_table_t;
page_table_t *vmm_create_address_space(void);
void vmm_map_page(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);

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
    
    printk("PIT Timer initialized at 100Hz.\n");
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
static void main_kthread(void) {
    printk("main kthread started.\n");
    char hi[3] = "hi";
    int fd_test = vfs_create_file(hi, "main.txt", 3);
    printk("bam: %d\n", fd_test);
    
    // FIX: Create an isolated, sandboxed user address space context.
    // This routine clones kernel space (entries 256-511) and switches CR3 automatically.
    page_table_t *user_pml4 = vmm_create_address_space();
    if (user_pml4 == NULL) {
        printk("Error: Could not allocate isolated user address space\n");
        for(;;);
    }
    
    // KEEP: Your preferred 32 MB clean virtual memory base address choice!
    uint64_t user_code_vma  = 0x2000000; 
    uint64_t user_stack_vma = 0x600000;

    int user_fd = vfs_open("user.bin");
    if (user_fd < 0) {
        printk("Error: Could not open user.bin\n");
        for(;;);
    }

    uint64_t user_flags = PTE_USER | PTE_WRITABLE;
    int page_index = 0;
    int total_bytes_read = 0;
    int file_cursor = 0;

    // Shift hardcoded physical targets high up into RAM (128 MB and 160 MB marks).
    // The PMM is explicitly configured to ignore these ranges so they never collide.
    uint64_t safe_code_phys_base  = 0x8000000; 
    uint64_t safe_stack_phys_base = 0xA000000; 

    // ==========================================
    // LOOP AND READ UNTIL END OF FILE (EOF)
    // ==========================================
    while (1) {
        // Calculate current physical frame and its corresponding HHDM virtual address
        uint64_t page_phys = safe_code_phys_base + (page_index * PAGE_SIZE);
        void *page_hhdm_ptr = (void *)(page_phys + HHDM_OFFSET);
        
        // Safely clear the target physical memory frame
        memset(page_hhdm_ptr, 0, PAGE_SIZE);

        // Read directly into our safe physical memory frame
        int chunks_read = vfs_read(user_fd, page_hhdm_ptr, PAGE_SIZE, file_cursor);

        if (chunks_read <= 0) {
            break; 
        }

        // Map this physical address to the user's isolated virtual memory space block
        uint64_t current_vma = user_code_vma + (page_index * PAGE_SIZE);
        vmm_map_page(user_pml4, current_vma, page_phys, user_flags);

        // Print header signature verification
        if (page_index == 0) {
            uint8_t *check = (uint8_t *)0x2000000;
            printk("VFS Payload Copied bytes: 0x%x 0x%x 0x%x 0x%x\n", check[0], check[1], check[2], check[3]);
        }

        total_bytes_read += chunks_read;
        file_cursor += chunks_read;
        page_index++;

        if (chunks_read < PAGE_SIZE) {
            break;
        }
    }

    printk("Loaded user binary from VFS: %d bytes read across %d pages.\n", total_bytes_read, page_index);
    vfs_free_fd(user_fd);

    if (total_bytes_read <= 0) {
        printk("Error: user.bin is empty or failed to load!\n");
        for(;;);
    }

    // ==========================================
    // MAP STACK TO SAFE PHYSICAL AREA
    // ==========================================
    uint8_t *check = (uint8_t *)0x2000000;
    printk("VFS Payload Copied bytes: 0x%x 0x%x 0x%x 0x%x\n", check[0], check[1], check[2], check[3]);
    void *stack_hhdm_ptr = (void *)(safe_stack_phys_base + HHDM_OFFSET);
    printk("VFS Payload Copied bytes: 0x%x 0x%x 0x%x 0x%x\n", check[0], check[1], check[2], check[3]);
    memset(stack_hhdm_ptr, 0, PAGE_SIZE);
    printk("VFS Payload Copied bytes: 0x%x 0x%x 0x%x 0x%x\n", check[0], check[1], check[2], check[3]);
    vmm_map_page(user_pml4, user_stack_vma, safe_stack_phys_base, user_flags);
    printk("VFS Payload Copied bytes: 0x%x 0x%x 0x%x 0x%x\n", check[0], check[1], check[2], check[3]);
    void *user_rsp = (void *)(user_stack_vma + PAGE_SIZE);
    
    // Fire off your context switch with scheduler interrupts active
    create_user_task((void *)0x2000000, user_rsp);
    printk("VFS Payload Copied bytes: 0x%x 0x%x 0x%x 0x%x\n", check[0], check[1], check[2], check[3]);
    
    for (;;) {
        asm volatile("sti; hlt");
    }
}
void dump_namespace(uacpi_namespace_node *root)
{
    if (!root)
        return;

    uacpi_namespace_node *node = root;

    while (node) {

        int depth = 0;
        uacpi_namespace_node *tmp = node->parent;

        while (tmp) {
            depth++;
            tmp = tmp->parent;
        }

        for (int i = 0; i < depth; i++)
            printk("  ");

        printk("%c%c%c%c\n",
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
/* SSE initialization removed: we do not enable OSFXSR/OSXMMEXCPT or touch MXCSR here. */
// Your Kernel Entry Point
void _start(void) {
    // Ensure the bootloader answered our framebuffer request safely
    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        hlt();
    }
    init_sse();
    gdt_init();
    idt_init();
    // FIX 1: Access the array base element pointer directly
    struct limine_framebuffer **framebuffers = framebuffer_request.response->framebuffers;
    struct limine_framebuffer *framebuffer = framebuffers[0];

    // FIX 2: Explicitly cast void* address to a 32-bit unsigned integer pointer
    uint32_t *fb_ptr = (uint32_t *)framebuffer->address;

    struct flanterm_context *ft_ctx = flanterm_fb_init(
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
        0, 0,
        0,
        0
    );
    memory_init();
    
    initConsole(ft_ctx);
    init_vfs();
    int *hello = kmalloc(sizeof(int) * 5);
    uacpi_status ret = uacpi_initialize(0);
    if (uacpi_unlikely_error(ret)) {
        printk("uacpi_initialize error: %s", uacpi_status_to_string(ret));
    }
    struct acpi_table_madt *madt = NULL;
    ret = uacpi_table_find_by_signature("APIC", (struct acpi_table**)&madt);
    if (ret == UACPI_STATUS_OK) {
        printk("Found MADT at %p\n", madt);
        // You can now parse your CPUs/Interrupt controllers here!
    } else {
        printk("MADT not found: %s\n", uacpi_status_to_string(ret));
    }
    ioapic(madt);
    asm volatile ("sti"); // Enable interrupts now that we're ready to handle them
    // Re-enable SSE features (OSFXSR/OSXMMEXCPT, MXCSR)
    keyboard_init();

    if (create_kernel_task(main_kthread) < 0) {
        printk("Failed to create main kthread.\n");
        hlt();
    }
    /* SSE support removed: not enabling OSFXSR/OSXMMEXCPT or MXCSR here */
    // CRITICAL FIX: Hand over control directly to your scheduler file instead of doing `sti` here.
    // Your start_scheduler() function in the other file must shift RSP to task_table[0].kernel_stack,
    // run pit_init(), and execute the final sti there.
    
    start_scheduler();
}
