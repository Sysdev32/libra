// SPDX-License-Identifier: GPL-3.0-only

#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include <vendor/flanterm/flanterm.h>
#include <vendor/flanterm/flanterm_backends/fb.h>
#include <drivers/fb.h>
#include <arch/x86_64/gdt.h>
#include <drivers/alloc.h>
#include <arch/x86_64/idt.h>
#include <arch/x86_64/schedule.h>
#include <fs/vfs.h>
#include <drivers/tty.h>
#include <hals/pci.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <uacpi/event.h>
#include <string.h>
#include <uacpi/tables.h>
#include <config.h>
#include <uacpi/internal/namespace.h>
#include <uacpi/types.h>
#include <errno.h>
#include <drivers/elf.h>
#include <fs/mnt.h>

#include "ioctl.h"
#include "hals/nvme.h"
#include "hals/ps2.h"
struct flanterm_context *ft_ctx;
extern char __user_src_start[];
extern char __user_src_end[];
#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_FRAME    0x000FFFFFFFFFF000ULL
#define HHDM_OFFSET  0xffff800000000000ULL
#define PAGE_SIZE 4096
#define ARENA_START 0x6000000ULL
#define ARENA_END   0x8000000ULL
#define PAGE_SIZE   0x1000ULL
#define NUM_PAGES ((ARENA_END - ARENA_START) / PAGE_SIZE)
#define MAX_ALLOCS 128
#define STACK_PHYS_START 0x0A200000ULL
#define STACK_PHYS_END   0x0C200000ULL   // 32 MiB for stacks

#define PAGE_SIZE   0x1000ULL
#define NUM_PAGES   ((STACK_PHYS_END - STACK_PHYS_START) / PAGE_SIZE)
#define MAX_STACKS  128

static uint8_t stack_bitmap[NUM_PAGES / 8];
typedef uint64_t page_table_t;
static uint8_t bitmap[NUM_PAGES / 8];

struct arena_alloc {
    void *addr;
    size_t pages;
    int used;
};

static struct arena_alloc allocs[MAX_ALLOCS];
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
int spawn(const char *path, int argc, char **argv, char* name)
{
    if (!path) {
        printk(LOG_ERROR, "[SPAWN] NULL path pointer\n");
        return -1;
    }
    if (argc < 0 || argc > 64 || (argc > 0 && argv == NULL)) {
        printk(LOG_ERROR, "[SPAWN] Invalid argv vector\n");
        return -1;
    }
    char kpath[256];
    memset(kpath, 0, sizeof(kpath));

    for (int i = 0; i < 255; i++) {
        char c = path[i];
        kpath[i] = c;

        if (c == '\0')
            break;

        if (i == 254) {
            kpath[255] = '\0';
        }
    }
    page_table_t *user_pml4 = vmm_create_address_space();

    if (!user_pml4) {
        printk(LOG_ERROR,
               "[SPAWN] Failed creating address space\n");
        for(;;);
    }


    int user_fd = vfs_open(kpath, O_RDONLY, 0);
    if (user_fd < 0) {
        printk(LOG_ERROR,
               "[SPAWN] Cannot open '%s'\n",
               kpath);
        for(;;);
    }


    struct vfs_stat stat = {0};

    vfs_fstat(user_fd, &stat);
    uint64_t user_flags = PTE_USER | PTE_WRITABLE;
    uint64_t safe_code_phys_base =
        (uint64_t)arena_alloc(stat.st_size);
    uint64_t safe_stack_phys_base =
        stack_alloc(256 * 1024);
    uint64_t raw_elf_phys_base =
        safe_code_phys_base + (9216 * PAGE_SIZE);


    void *raw_elf_hhdm_ptr =
        (void *)(raw_elf_phys_base + HHDM_OFFSET);
    int file_cursor = 0;
    uint64_t total_bytes_read = 0;

    while (1)
    {
        void *dst =
            (void *)((uint8_t *)raw_elf_hhdm_ptr +
                     total_bytes_read);


        int read =
            vfs_read(user_fd,
                     dst,
                     PAGE_SIZE,
                     file_cursor);


        if (read <= 0)
            break;


        total_bytes_read += read;
        file_cursor += read;
    }

    uint64_t user_code_vma =
        elf_vaddr(raw_elf_hhdm_ptr);
    vfs_free_fd(user_fd);


    int staging_pages = 8300;


    for (int i = 0; i < staging_pages; i++)
    {
        uint64_t phys =
            safe_code_phys_base +
            i * PAGE_SIZE;


        uint64_t virt =
            user_code_vma +
            i * PAGE_SIZE;

        memset((void *)(phys + HHDM_OFFSET),
               0,
               PAGE_SIZE);


        vmm_map_page(user_pml4,
                     virt,
                     phys,
                     user_flags);
    }



    ElfLoadResult loaded =
        load_elf(raw_elf_hhdm_ptr,
                 safe_code_phys_base,
                 user_code_vma);

    if (!loaded.entry_point)
    {
        printk(LOG_ERROR,
               "[SPAWN] ELF loader failed\n");
        for(;;);
    }




    uint64_t user_stack_vma = 0x600000;


    int stack_pages = 64;


    for (int i = 0; i < stack_pages; i++)
    {
        uint64_t virt =
            user_stack_vma +
            i * PAGE_SIZE;


        uint64_t phys =
            safe_stack_phys_base +
            i * PAGE_SIZE;

        memset((void *)(phys + HHDM_OFFSET),
               0,
               PAGE_SIZE);


        vmm_map_page(user_pml4,
                     virt,
                     phys,
                     user_flags);
    }
    uint64_t stack_top_vma =
        user_stack_vma +
        stack_pages * PAGE_SIZE;


    uint64_t stack_top_hhdm =
        safe_stack_phys_base +
        stack_pages * PAGE_SIZE +
        HHDM_OFFSET;

    uint64_t cur_vma = stack_top_vma;
    uint64_t cur_hhdm = stack_top_hhdm;


    uint64_t argv_ptrs[64];


    for (int i = argc - 1; i >= 0; i--)
    {
        if (argv[i] == NULL) {
            printk(LOG_ERROR, "[SPAWN] NULL argv[%d]\n", i);
            return -1;
        }

        size_t len =
            strlen(argv[i]) + 1;


        cur_vma -= len;
        cur_hhdm -= len;


        memcpy((void *)cur_hhdm,
               argv[i],
               len);


        argv_ptrs[i] = cur_vma;
    }



    cur_vma &= -8ULL;
    cur_hhdm &= -8ULL;

    uint64_t stack_words = (uint64_t)argc + 2;
    if (((cur_vma - (stack_words * 8)) & 0xFULL) != 0) {
        cur_vma -= 8;
        cur_hhdm -= 8;
    }

    cur_vma -= 8;
    cur_hhdm -= 8;

    *(uint64_t *)cur_hhdm = 0;


    for(int i = argc - 1; i >= 0; i--)
    {
        cur_vma -= 8;
        cur_hhdm -= 8;

        *(uint64_t *)cur_hhdm =
            argv_ptrs[i];
    }


    uint64_t argv_start =
        cur_vma;


    cur_vma -= 8;
    cur_hhdm -= 8;


    *(uint64_t *)cur_hhdm =
        argc;

    set_cwd(getpcwd());
    int pid =
        create_user_task(
            (void *)loaded.entry_point,
            (void *)cur_vma,
            argc,
            argv_start,
            user_pml4,
            0,
            0,
            -1,
            name);


    return pid;
}
extern struct process process_table[MAX_PROCESSES];
extern struct thread thread_table[MAX_THREADS];

extern volatile int current_thread_id;
int clone(void (*fn)(void *), void *arg, int argc, bool is_user)
{
    if (!fn) {
        printk(LOG_ERROR, "[CLONE] NULL entry point function\n");
        return -1;
    }

    struct process *current_proc = get_current_proc();
    if (!current_proc) {
        printk(LOG_ERROR, "[CLONE] Failed to retrieve current process\n");
        return -1;
    }

    uint64_t child_stack = 0;

    if (is_user) {
        // Allocate a dedicated 256KB user stack for the child thread
        uint64_t phys_stack = stack_alloc(256 * 1024);
        if (!phys_stack) {
            printk(LOG_ERROR, "[CLONE] Failed to allocate user stack\n");
            return -1;
        }

        uint64_t user_stack_vma = 0x700000000000ULL + (current_thread_id * 0x40000); // Unique VMA space
        uint64_t user_flags = PTE_USER | PTE_WRITABLE;

        for (int i = 0; i < 64; i++) {
            uint64_t virt = user_stack_vma + i * PAGE_SIZE;
            uint64_t phys = phys_stack + i * PAGE_SIZE;

            memset((void *)(phys + HHDM_OFFSET), 0, PAGE_SIZE);
            vmm_map_page(current_proc->pml4, virt, phys, user_flags);
        }

        // Set child stack top aligned to 16 bytes
        child_stack = (user_stack_vma + 64 * PAGE_SIZE) & ~0xFULL;
    }

    // Pass the caller's current fs_base so the child inherits TLS context
    uint64_t parent_fs_base = thread_table[current_thread_id].fs_base;

    int tid = create_thread(
        current_proc,             // Process context
        (void (*)(void))fn,       // Entry function
        (void *)child_stack,      // Isolated stack pointer (0 for kernel threads)
        (uint64_t)arg,            // Passed in RDI
        (uint64_t)argc,           // Passed in RSI
        parent_fs_base,           // Inherited TLS base (IA32_FS_BASE)
        is_user                   // Privilege level flag
    );

    if (tid < 0) {
        printk(LOG_ERROR, "[CLONE] Failed to clone thread under PID %d\n", current_proc->pid);
        return -1;
    }

    printk(LOG_TRACE, "[CLONE] Successfully spawned %s thread TID %d under PID %d\n",
           is_user ? "user" : "kernel", tid, current_proc->pid);

    return tid;
}
int launchd_pid = -1;

#define TLS_KEY_MY_VAR 0

static void test(void* arg) {
    int thread_id = (int)(uintptr_t)arg;
    printk(LOG_ERROR, "thread id: %d\n", thread_id);

    // Set a unique value for this specific thread using the TLS slot
    uint64_t expected_val = (uint64_t)thread_id * 100;
    set_tls(TLS_KEY_MY_VAR, expected_val);

    for (int i = 0; i < 3; i++) {
        uint64_t current_val = get_tls(TLS_KEY_MY_VAR);

        printk(LOG_TRACE, "[THREAD %d] TLS Var Value: %lu\n",
               thread_id, current_val);

        if (current_val != expected_val) {
            printk(LOG_ERROR, "[THREAD %d] FAIL! TLS variable corrupted! (Expected %lu, Got %lu)\n",
                   thread_id, expected_val, current_val);
           for (;;);
        }
    }

    printk(LOG_TRACE, "[THREAD %d] SUCCESS! TLS variable stayed intact.\n", thread_id);
    for (;;);
}
static void main_kthread(void) {
    clone(test, 1, NULL, false);
    clone(test, 2, NULL, false);
    launchd_pid = spawn("/System/usr/bin/commandline/launchd", 0, NULL, "launchd");
    for (;;) {
        asm volatile("sti; hlt");
    }
}
unsigned long long get_launchd_pid() {
    return launchd_pid;
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
static void tty_echo_off(char* path) {
    int fd = open(path, 2, 0);
    if (fd >= 0) {
        struct winsize wz;
        if (ioctl(fd, TIOCGWINSZ, &wz) == 0) {
            printk(LOG_TRACE, "%dx%d\n", wz.ws_col, wz.ws_row);
        }

        struct termios t;
        if (ioctl(fd, TCGETS, &t) == 0) {
            t.c_lflag &= ~ECHO;
            ioctl(fd, TCSETS, &t);
        }
    }
}
pci_device_t* devices;
uint32_t devicecount = 0;

void tests(void);

// ReSharper disable once CppUseInternalLinkage
void _start(void) { // NOLINT(*-reserved-identifier)
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

    struct limine_framebuffer **framebuffers = framebuffer_request.response->framebuffers;
    struct limine_framebuffer *framebuffer = framebuffers[0];

    uint32_t *fb_ptr = (uint32_t *)framebuffer->address;
    framebuffer_t fbt;
    fbt.address = fb_ptr;
    fbt.height = framebuffer->height;
    fbt.pitch = framebuffer->pitch;
    fbt.width = framebuffer->width;
    memory_init();
    keyboard_init();
    tty_init(&fbt);
    tty_switch(1);
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
    // ReSharper disable once CppIncompatiblePointerConversion
    ret = uacpi_table_find_by_signature("APIC", (struct uacpi_table*)&madt);
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
    devices = kcalloc(256, sizeof(pci_device_t));
    
    pci_scan_bus(devices, 256, &devicecount);
    for (int i=0; i<devicecount; i++) {
        printk(LOG_INFO, "PCI DEVICE: %d:%d:%d %x:%x %x:%x\n", devices[i].bus, devices[i].device, devices[i].function, devices[i].class_code, devices[i].subclass, devices[i].device_id, devices[i].vendor_id);
    }
    nvme_init();
    tests();
    partition_t dev;
    dev.type = DEVFS;
    mount(&dev, "/dev");
    tty_dev_init();
    // Echo bugs out on userspace turnoff
    tty_echo_off("/dev/tty1");
    tty_echo_off("/dev/tty2");
    tty_echo_off("/dev/tty3");
    tty_echo_off("/dev/tty4");
    tty_echo_off("/dev/tty5");
    tty_echo_off("/dev/tty6");
    create_kernel_task(main_kthread, "khost");
    start_scheduler();
    for(;;);

}
