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
#include <hals/virtio/virtio_gpu.h>

#include "hals/ehci.h"
#include "hals/xhci.h"
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
/* Standard translation helper if you don't have virt_to_phys */
#define VIRT_TO_PHYS(addr) ((uint64_t)(addr) - HHDM_OFFSET)
int spawn(const char *path, int argc, char **argv, char* name)
{
    printk(LOG_DEBUG, "\n================ [SPAWN DEBUG START] ================\n");
    printk(LOG_DEBUG, "[SPAWN] Path: '%s' | argc: %d | Task Name: '%s'\n",
           path ? path : "NULL", argc, name ? name : "NULL");

    if (!path) {
        printk(LOG_ERROR, "[SPAWN ERROR] NULL path pointer\n");
        return -1;
    }
    if (argc < 0 || argc > 64 || (argc > 0 && argv == NULL)) {
        printk(LOG_ERROR, "[SPAWN ERROR] Invalid argv vector\n");
        return -1;
    }

    char kpath[256];
    memset(kpath, 0, sizeof(kpath));

    for (int i = 0; i < 255; i++) {
        char c = path[i];
        kpath[i] = c;
        if (c == '\0') break;
        if (i == 254) kpath[255] = '\0';
    }

    /* 1. Create Isolated PML4 */
    printk(LOG_DEBUG, "[SPAWN] Creating user PML4...\n");
    page_table_t *user_pml4 = vmm_create_address_space();
    if (!user_pml4) {
        printk(LOG_ERROR, "[SPAWN ERROR] Failed creating user PML4 address space!\n");
        return -1;
    }

    uint64_t pml4_phys = (uint64_t)user_pml4;
    if (pml4_phys >= HHDM_OFFSET) {
        pml4_phys -= HHDM_OFFSET;
    }
    printk(LOG_DEBUG, "[SPAWN] User PML4 HHDM: %p | PML4 Phys: %x\n", user_pml4, pml4_phys);

    /* 2. Read Executable into Buffer */
    int user_fd = vfs_open(kpath, O_RDONLY, 0);
    if (user_fd < 0) {
        printk(LOG_ERROR, "[SPAWN ERROR] Cannot open binary '%s'\n", kpath);
        return -1;
    }

    struct vfs_stat stat = {0};
    vfs_fstat(user_fd, &stat);
    printk(LOG_DEBUG, "[SPAWN] Executable file size: %d bytes\n", stat.st_size);

    if (stat.st_size == 0) {
        printk(LOG_ERROR, "[SPAWN ERROR] File '%s' is 0 bytes\n", kpath);
        vfs_free_fd(user_fd);
        return -1;
    }

    uint8_t *raw_elf_buf = (uint8_t *)kmalloc(stat.st_size);
    if (!raw_elf_buf) {
        printk(LOG_ERROR, "[SPAWN ERROR] Failed to kmalloc %d bytes for ELF buffer!\n", stat.st_size);
        vfs_free_fd(user_fd);
        return -1;
    }

    int file_cursor = 0;
    uint64_t total_bytes_read = 0;

    while (total_bytes_read < (uint64_t)stat.st_size)
    {
        int read = vfs_read(user_fd, raw_elf_buf + total_bytes_read, PAGE_SIZE, file_cursor);
        if (read <= 0) break;
        total_bytes_read += read;
        file_cursor += read;
    }
    vfs_free_fd(user_fd);
    printk(LOG_DEBUG, "[SPAWN] Read %d bytes into kernel memory successfully.\n", (int)total_bytes_read);

    uint64_t user_code_vma = elf_vaddr((void *)raw_elf_buf);
    uint64_t user_flags = PTE_USER | PTE_WRITABLE;
    printk(LOG_DEBUG, "[SPAWN] ELF target Base Virtual Address (VMA): %x\n", user_code_vma);

    /* 3. Dynamic ELF Allocation & Loading */
    /* Map only the program size rounded up to page boundary, NOT 256MB */
    size_t elf_pages = (stat.st_size + PAGE_SIZE - 1) / PAGE_SIZE;
    /* Add extra pages for BSS/Data segments */
    elf_pages += 16;

    printk(LOG_DEBUG, "[SPAWN] Mapping %d pages for executable segments...\n", (int)elf_pages);

    for (size_t i = 0; i < elf_pages; i++)
    {
        void *raw_ptr = pmm_alloc_pages(0);
        if (!raw_ptr) {
            printk(LOG_ERROR, "[SPAWN ERROR] Out of physical memory loading binary segments!\n");
            kfree(raw_elf_buf);
            return -1;
        }

        uint64_t page_phys = (uint64_t)raw_ptr >= HHDM_OFFSET ? (uint64_t)raw_ptr - HHDM_OFFSET : (uint64_t)raw_ptr;
        void *page_hhdm_ptr = (void *)(page_phys + HHDM_OFFSET);

        uint64_t virt = user_code_vma + (i * PAGE_SIZE);

        memset(page_hhdm_ptr, 0, PAGE_SIZE);
        vmm_map_page(user_pml4, virt, page_phys, user_flags);
    }

    printk(LOG_DEBUG, "[SPAWN] Parsing ELF headers via load_elf()...\n");
    ElfLoadResult loaded = load_elf(raw_elf_buf, user_pml4, user_code_vma);

    kfree(raw_elf_buf);

    if (!loaded.entry_point) {
        printk(LOG_ERROR, "[SPAWN ERROR] ELF loader returned NULL entry point!\n");
        return -1;
    }
    printk(LOG_DEBUG, "[SPAWN] ELF parsed! Target RIP: %x\n", loaded.entry_point);

    /* 4. Map User Stack (64 KB) */
    uint64_t user_stack_vma = 0x600000;
    int stack_pages = 16; // 64 KB is plenty for process init
    uint64_t stack_top_hhdm = 0;

    printk(LOG_DEBUG, "[SPAWN] Allocating %d stack pages at VMA: %x...\n", stack_pages, user_stack_vma);

    for (int i = 0; i < stack_pages; i++)
    {
        void *raw_ptr = pmm_alloc_pages(0);
        if (!raw_ptr) {
            printk(LOG_ERROR, "[SPAWN ERROR] Failed allocating stack page!\n");
            return -1;
        }

        uint64_t stack_phys = (uint64_t)raw_ptr >= HHDM_OFFSET ? (uint64_t)raw_ptr - HHDM_OFFSET : (uint64_t)raw_ptr;
        void *stack_hhdm_ptr = (void *)(stack_phys + HHDM_OFFSET);
        uint64_t virt = user_stack_vma + (i * PAGE_SIZE);

        memset(stack_hhdm_ptr, 0, PAGE_SIZE);
        vmm_map_page(user_pml4, virt, stack_phys, user_flags);

        if (i == stack_pages - 1) {
            stack_top_hhdm = (uint64_t)stack_hhdm_ptr + PAGE_SIZE;
        }
    }

    uint64_t cur_vma = user_stack_vma + (stack_pages * PAGE_SIZE);
    uint64_t cur_hhdm = stack_top_hhdm;

    /* 5. Push Arguments onto Stack */
    uint64_t argv_ptrs[64];

    for (int i = argc - 1; i >= 0; i--)
    {
        if (argv[i] == NULL) {
            printk(LOG_ERROR, "[SPAWN ERROR] NULL argv[%d]\n", i);
            return -1;
        }

        size_t len = strlen(argv[i]) + 1;
        cur_vma -= len;
        cur_hhdm -= len;

        memcpy((void *)cur_hhdm, argv[i], len);
        argv_ptrs[i] = cur_vma;
    }

    /* 16-byte align stack pointer */
    cur_vma &= -16ULL;
    cur_hhdm &= -16ULL;

    cur_vma -= 8;
    cur_hhdm -= 8;
    *(uint64_t *)cur_hhdm = 0; // NULL terminator

    for (int i = argc - 1; i >= 0; i--)
    {
        cur_vma -= 8;
        cur_hhdm -= 8;
        *(uint64_t *)cur_hhdm = argv_ptrs[i];
    }

    uint64_t argv_start = cur_vma;

    cur_vma -= 8;
    cur_hhdm -= 8;
    *(uint64_t *)cur_hhdm = argc;

    printk(LOG_DEBUG, "[SPAWN STACK] Final RSP VMA: %x | argv_start: %x\n", cur_vma, argv_start);

    /* 6. Context Switch Task Creation */
    int pid = create_user_task(
        (void *)loaded.entry_point,
        (void *)cur_vma,
        argc,
        argv_start,
        user_pml4,
        0,
        0,
        -1,
        name
    );

    printk(LOG_DEBUG, "[SPAWN SUCCESS] Task created with PID: %d\n", pid);
    printk(LOG_DEBUG, "================ [SPAWN DEBUG END] ================\n\n");

    return pid;
}
extern struct process process_table[MAX_PROCESSES];
extern struct thread thread_table[MAX_THREADS];

extern volatile int current_thread_id;
int execve(const char *path, char *const argv[], char *const envp[], uint64_t current_rsp) {
    (void)envp; // Unused for now

    // 1. Validate inputs and current execution context
    if (!path) {
        printk(LOG_ERROR, "[EXECVE] NULL path pointer\n");
        return -1;
    }

    struct thread *curr_thread = &thread_table[current_thread_id];
    struct process *curr_proc = curr_thread->process;

    if (!curr_proc || !curr_thread) {
        return -1;
    }

    // Safely parse target path
    char kpath[256];
    memset(kpath, 0, sizeof(kpath));
    for (int i = 0; i < 255; i++) {
        char c = path[i];
        kpath[i] = c;
        if (c == '\0') break;
        if (i == 254) kpath[255] = '\0';
    }

    // Count and parse argv vector safely
    int argc = 0;
    if (argv != NULL) {
        while (argv[argc] != NULL) {
            argc++;
            if (argc > 64) {
                printk(LOG_ERROR, "[EXECVE] Too many arguments\n");
                return -1;
            }
        }
    }

    // 2. Open executable binary from VFS
    int user_fd = vfs_open(kpath, O_RDONLY, 0);
    if (user_fd < 0) {
        printk(LOG_ERROR, "[EXECVE] Cannot open binary '%s'\n", kpath);
        return -1;
    }

    // 3. Create fresh user page table PML4
    page_table_t *new_pml4 = vmm_create_address_space();
    if (!new_pml4) {
        printk(LOG_ERROR, "[EXECVE] Failed to allocate PML4\n");
        vfs_free_fd(user_fd);
        return -1;
    }

    // 4. Allocate memory buffers & read raw ELF binary into memory
    struct vfs_stat stat = {0};
    vfs_fstat(user_fd, &stat);
    uint64_t user_flags = PTE_USER | PTE_WRITABLE;

    uint64_t safe_code_phys_base = (uint64_t)arena_alloc(stat.st_size);
    uint64_t safe_stack_phys_base = stack_alloc(256 * 1024);
    uint64_t raw_elf_phys_base = safe_code_phys_base + (9216 * PAGE_SIZE);

    void *raw_elf_hhdm_ptr = (void *)(raw_elf_phys_base + HHDM_OFFSET);
    int file_cursor = 0;
    uint64_t total_bytes_read = 0;

    while (1) {
        void *dst = (void *)((uint8_t *)raw_elf_hhdm_ptr + total_bytes_read);
        int read = vfs_read(user_fd, dst, PAGE_SIZE, file_cursor);
        if (read <= 0) break;
        total_bytes_read += read;
        file_cursor += read;
    }

    uint64_t user_code_vma = elf_vaddr(raw_elf_hhdm_ptr);
    vfs_free_fd(user_fd);

    // 5. Map code & staging pages into new virtual address space
    int staging_pages = 8300;
    for (int i = 0; i < staging_pages; i++) {
        uint64_t phys = safe_code_phys_base + i * PAGE_SIZE;
        uint64_t virt = user_code_vma + i * PAGE_SIZE;
        memset((void *)(phys + HHDM_OFFSET), 0, PAGE_SIZE);
        vmm_map_page(new_pml4, virt, phys, user_flags);
    }

    ElfLoadResult loaded = load_elf(raw_elf_hhdm_ptr, safe_code_phys_base, user_code_vma);
    if (!loaded.entry_point) {
        printk(LOG_ERROR, "[EXECVE] ELF loading failed\n");
        return -1;
    }

    // 6. Map and setup user stack pages
    uint64_t user_stack_vma = 0x600000;
    int stack_pages = 64;

    for (int i = 0; i < stack_pages; i++) {
        uint64_t virt = user_stack_vma + i * PAGE_SIZE;
        uint64_t phys = safe_stack_phys_base + i * PAGE_SIZE;
        memset((void *)(phys + HHDM_OFFSET), 0, PAGE_SIZE);
        vmm_map_page(new_pml4, virt, phys, user_flags);
    }

    uint64_t stack_top_vma = user_stack_vma + stack_pages * PAGE_SIZE;
    uint64_t stack_top_hhdm = safe_stack_phys_base + stack_pages * PAGE_SIZE + HHDM_OFFSET;

    uint64_t cur_vma = stack_top_vma;
    uint64_t cur_hhdm = stack_top_hhdm;
    uint64_t argv_ptrs[64];

    // Push argument strings to user stack space
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        cur_vma -= len;
        cur_hhdm -= len;
        safe_memcpy((void *)cur_hhdm, argv[i], len);
        argv_ptrs[i] = cur_vma;
    }

    // Align stack boundary (16-byte ABI alignment)
    cur_vma &= -8ULL;
    cur_hhdm &= -8ULL;

    uint64_t stack_words = (uint64_t)argc + 2;
    if (((cur_vma - (stack_words * 8)) & 0xFULL) != 0) {
        cur_vma -= 8;
        cur_hhdm -= 8;
    }

    // Push NULL string terminator pointer
    cur_vma -= 8;
    cur_hhdm -= 8;
    *(uint64_t *)cur_hhdm = 0;

    // Push argv array pointers onto stack
    for (int i = argc - 1; i >= 0; i--) {
        cur_vma -= 8;
        cur_hhdm -= 8;
        *(uint64_t *)cur_hhdm = argv_ptrs[i];
    }

    uint64_t argv_start = cur_vma;

    // Push argc onto stack top
    cur_vma -= 8;
    cur_hhdm -= 8;
    *(uint64_t *)cur_hhdm = argc;

    // 7. POSIX behavior: Terminate sibling threads belonging to this process
    uint64_t flags = irq_save();
    for (int i = 0; i < MAX_THREADS; i++) {
        if (i != current_thread_id && thread_table[i].process == curr_proc) {
            thread_table[i].state = TASK_STATE_DEAD;
            thread_table[i].rsp = 0;
            thread_table[i].user_rsp = 0;
            thread_table[i].joining_tid = -1;
        }
    }

    // 8. Replace address space (PML4) & update current process metadata
    curr_proc->pml4 = new_pml4;

    // Extract base filename for process name
    const char *last_slash = kpath;
    for (const char *p = kpath; *p; p++) {
        if (*p == '/') last_slash = p + 1;
    }
    strcpy(curr_proc->name, last_slash);

    // Reset pending signals and IPC queues
    curr_proc->pending_signals = 0;
    curr_proc->msg_head = 0;
    curr_proc->msg_tail = 0;
    curr_proc->msg_count = 0;

    // 9. Reset thread control state & overwrite trap frame to jump to entry point
    init_fpu_context(curr_thread);
    curr_thread->user_rsp = cur_vma;
    curr_thread->fs_base = 0;
    curr_thread->gs_base = 0;

    // Switch active page tables immediately to child space
    uint64_t new_cr3_phys = (uint64_t)new_pml4 - HHDM_OFFSET;
    asm volatile("mov %0, %%cr3" :: "r"(new_cr3_phys) : "memory");

    // Overwrite the interrupt context frame at current_rsp to jump directly into new entry
    uint64_t *ctx = (uint64_t *)current_rsp;
    memset(ctx, 0, 24 * sizeof(uint64_t));

    ctx[4]  = argv_start;             // RSI = argv
    ctx[5]  = (uint64_t)argc;         // RDI = argc
    ctx[17] = loaded.entry_point;     // RIP = executable entry point
    ctx[18] = 0x1B;                   // CS  = User Code Segment (0x1B)
    ctx[19] = 0x202;                  // RFLAGS = Enable Interrupts
    ctx[20] = cur_vma;                // RSP = New User Stack Pointer
    ctx[21] = 0x23;                   // SS  = User Data Segment (0x23)

    irq_restore(flags);

    printk(LOG_TRACE, "[EXECVE] Process PID %d executing new binary '%s'\n", curr_proc->pid, curr_proc->name);

    // Return current_rsp frame; kernel ISR return loop jumps into the new process entry
    return 0;
}

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define PTE_PRESENT   (1ULL << 0)
#define PTE_HUGE      (1ULL << 7)   /* PS bit in PDPT (1GB) or PD (2MB) */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL


/**
 * Translates a virtual address to a physical address by parsing the PML4 tree.
 *
 * @param pml4_phys Physical address of the PML4 root table for the target address space.
 * @param virt      Virtual address to translate.
 * @return Physical address, or 0 if unmapped/not present.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Page Table Entry Flags */
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_HUGE      (1ULL << 7)   /* 1GB in PDPT, 2MB in PD */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/*
 * Choose the recursive slot used when setting up PML4.
 * Slot 510 (0x1FE) is standard in x86_64 kernels.
 */
#define RECURSIVE_SLOT 510ULL

/**
 * Helper to construct canonical virtual addresses for recursive table access.
 */
static inline uint64_t get_recursive_virt(uint64_t l4, uint64_t l3, uint64_t l2, uint64_t l1) {
    uint64_t raw = (l4 << 39) | (l3 << 30) | (l2 << 21) | (l1 << 12);
    // Sign-extend bit 47 for canonical x86_64 address compliance
    if (raw & (1ULL << 47)) {
        raw |= 0xFFFF000000000000ULL;
    }
    return raw;
}


int clone(void (*fn)(void *), void *user_stack, void *arg, bool is_user)
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
    uint64_t child_fs_base = 0;

    if (is_user && user_stack != NULL) {
        // 1. Align top of user stack to 16-byte boundary
        uint64_t stack_top = (uint64_t)user_stack & ~0xFULL;

        // 2. Reserve space for the TCB frame at top of stack, maintaining 16-byte alignment
        child_fs_base = (stack_top - sizeof(struct tcb)) & ~0xFULL;

        // Translate user virtual address to physical address via direct page walk
        uintptr_t phys_tcb = hal_virt_to_phys((void *)child_fs_base);
        if (!phys_tcb) {
            printk(LOG_ERROR, "[CLONE] Stack address unmapped or invalid: 0x%x\n", child_fs_base);
            return -1;
        }

        struct tcb *child_tcb = (struct tcb *)(phys_tcb + HHDM_OFFSET);

        // Zero TCB block and set self-pointer (%fs:0x0)
        memset(child_tcb, 0, sizeof(struct tcb));
        child_tcb->self = (struct tcb *)child_fs_base;

        // 3. System V AMD64 ABI: (RSP + 8) must be 16-byte aligned before call/entry point
        // Position RSP below the TCB frame with proper alignment
        child_stack = (child_fs_base & ~0xFULL) - 8;
    }

    // 4. Dispatch to thread setup routine
    int tid = create_thread(
        current_proc,             // Process context
        (void (*)(void))fn,       // RIP (entry point)
        (void *)child_stack,      // RSP (top of user stack)
        (uint64_t)arg,            // RDI (ctx passed to trampoline)
        0,                        // RSI (unused/zeroed)
        child_fs_base,            // FS_BASE
        is_user                   // User mode flag
    );

    if (tid < 0) {
        printk(LOG_ERROR, "[CLONE] Failed to clone thread under PID %d\n", current_proc->pid);
        return -1;
    }

    // Set assigned thread ID inside user TCB
    if (is_user && child_fs_base) {
        uintptr_t phys_tcb = hal_virt_to_phys((void *)child_fs_base);
        if (phys_tcb) {
            struct tcb *child_tcb = (struct tcb *)(phys_tcb + HHDM_OFFSET);
            child_tcb->thread_id = tid;
        } else {
            printk(LOG_WARNING, "[CLONE] Unable to write thread_id to user TCB (0x%x translation failed)\n", child_fs_base);
        }
    }

    return tid;
}

int launchd_pid = -1;

#define TLS_KEY_MY_VAR 0

static int test(void* arg) {
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
    return 0;
}
static void main_kthread(void) {
    launchd_pid = spawn("/usr/bin/launchd", 0, NULL, "launchd");
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
virtio_gpu_device_t g_virtio_gpu;
virtio_device_t vdev;
void tests(void);
bool check_device(int i, int class, int subclass, int prog_if) {
    if (devices[i].class_code == class && devices[i].subclass == subclass && devices[i].prog_if == prog_if) {
        return true;
    }
    return false;
}
bool check_gpt(const void* buffer) {
    if (!buffer) return false;

    // Safest & cleanest approach: byte comparison against "EFI PART"
    return memcmp(buffer, "EFI PART", 8) == 0;
}
int check_fat32(volume_t* vol) {
    if (!vol || !vol->is_valid) return 0;

    uint32_t sector_size = (vol->drive.sector_size > 0) ? vol->drive.sector_size : 512;
    uint8_t* buf = (uint8_t*)kmalloc(sector_size);
    if (!buf) return 0;

    if (!volume_read_sectors(vol, 0, 1, buf)) {
        kfree(buf);
        return 0;
    }

    // Direct byte offset parsing without extra struct definitions
    uint16_t bytes_per_sector   = buf[11] | (buf[12] << 8);
    uint16_t root_entry_count   = buf[17] | (buf[18] << 8);
    uint16_t table_size_16      = buf[22] | (buf[23] << 8);
    uint32_t table_size_32      = buf[36] | (buf[37] << 8) | (buf[38] << 16) | (buf[39] << 24);

    // 1. Boot sector signature check (0x55, 0xAA)
    if (buf[510] != 0x55 || buf[511] != 0xAA) {
        kfree(buf);
        return 0;
    }

    // 2. Validate basic FAT32 properties (FAT16/12 legacy entries must be 0)
    if (bytes_per_sector != sector_size || root_entry_count != 0 || table_size_16 != 0 || table_size_32 == 0) {
        kfree(buf);
        return 0;
    }

    // 3. Verify string signature "FAT32   " at offset 0x52 (82)
    if (memcmp(&buf[82], "FAT32   ", 8) != 0) {
        kfree(buf);
        return 0;
    }

    kfree(buf);
    return 1;
}
initialized_drive init_drives[32];
int init_drives_count = 0;
static char* append_uint(char* dest, unsigned int val) {
    char temp[12];
    int i = 0;

    if (val == 0) {
        *dest++ = '0';
        return dest;
    }

    while (val > 0) {
        temp[i++] = '0' + (val % 10);
        val /= 10;
    }

    while (i > 0) {
        *dest++ = temp[--i];
    }

    return dest;
}

void make_nv_part_name(char *buf, unsigned int nv_id, unsigned int part_id) {
    *buf++ = '/'; // Add leading slash for absolute path lookup!
    *buf++ = 'n';
    *buf++ = 'v';
    buf = append_uint(buf, nv_id);
    *buf++ = 'p';
    *buf++ = 'a';
    *buf++ = 'r';
    *buf++ = 't';
    buf = append_uint(buf, part_id);
    *buf = '\0';
}
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
    if (total_usable_memory / 1024 / 1024 < 128) {
        printk(LOG_ERROR, "Less than 128 MB of usable memory detected. Rebooting now..\n");
        triple_fault_reboot();
    }
    init_vfs();
    
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
        if (devices[i].vendor_id == 0x1AF4 && devices[i].device_id == 0x1050) {
            virtio_init_device(&vdev, devices[i].bus, devices[i].device, devices[i].function);
            virtio_gpu_init(&g_virtio_gpu, &vdev, 1280, 720, framebuffer->address);
            tty_switch_gpu();
        }
        if (check_device(i, 0x01, 0x08, 0x02)) {
            int32_t ret = nvme_init(devices[i].bus, devices[i].device, devices[i].function);
            if (ret < 0) {
                printk(LOG_ERROR, "failed to initialize NVMe controller.\n");
            } else {
                nvme_namespace_t ns[32];
                int32_t count = nvme_get_namespaces(ret, ns, 32);
                printk(LOG_INFO, "namespace count: %d\n", count);

                // Renamed loop variable to ns_idx to avoid shadowing outer 'i'
                for (int ns_idx = 0; ns_idx < count; ns_idx++) {
                    initialized_drive drive;
                    memset(&drive, 0, sizeof(initialized_drive));

                    uint32_t nsid = ns[ns_idx].nsid;
                    uint32_t sector_size = nvme_get_sector_size(ret, nsid);

                    if (sector_size == 0) {
                        printk(LOG_ERROR, "failed to retrieve sector size for NSID %d.\n", nsid);
                        continue;
                    }

                    void* buffer = kmalloc(sector_size);
                    if (!buffer) {
                        printk(LOG_ERROR, "memory allocation failed for NSID %d sector buffer.\n", nsid);
                        continue;
                    }

                    // Read LBA 1 (GPT Header)
                    if (nvme_read_block(ret, nsid, 1, 1, buffer)) {
                        printk(LOG_INFO, "nsid %d, sector size: %d bytes.\n", nsid, sector_size);

                        // Log the first 16 bytes of LBA 1 in HEX
                        uint8_t* raw = (uint8_t*)buffer;
                        printk(LOG_INFO, "LBA 1 Header (first 16 bytes): %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                               raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
                               raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);

                        if (check_gpt(buffer)) {
                            printk(LOG_INFO, "detected GPT formatting scheme on nsid: %d\n", nsid);

                            generic_drive_t gpt_drive;
                            gpt_drive.sector_size = ns[ns_idx].sector_size;
                            gpt_drive.total_sectors = ns[ns_idx].sector_count;
                            gpt_drive.type = DRIVE_TYPE_NVME;
                            gpt_drive.nvme.nsid = nsid;
                            gpt_drive.nvme.nvme_id = ret;
                            partition_table_t table = gpt_parse_partitions(&gpt_drive);

                            printk(LOG_INFO, "GPT partition count for nsid %d: %d\n", nsid, table.count);

                            drive.format.gpt_partition_table.count = table.count;
                            drive.drive = gpt_drive;
                            drive.formatType = FS_GPT;

                            for (int l = 0; l < table.count; l++) {
                                printk(LOG_INFO, "evaluating partition %d (LBA start: %llu, count: %llu)...\n",
                                       l, table.partitions[l]->start_lba, table.partitions[l]->total_sectors);

                                if (check_fat32(table.partitions[l])) {
                                    printk(LOG_INFO, "partition %d is FAT32. Initializing filesystem...\n", l);

                                    fat32_fs_t fs;
                                    fat32_init(table.partitions[l], &fs);

                                    drive.format.gpt_partition_table.vols[l].base = *table.partitions[l];
                                    drive.format.gpt_partition_table.vols[l].fsType = FS_FAT32;
                                    drive.format.gpt_partition_table.vols[l].fs.filesystem = fs;
                                    char name[32];
                                    make_nv_part_name(name, ret, l);
                                } else {
                                    printk(LOG_INFO, "partition %d is not FAT32.\n", l);
                                }
                            }

                            init_drives[init_drives_count++] = drive;
                        } else {
                            printk(LOG_ERROR, "unsupported formatting scheme on nsid %d.\n", nsid);
                        }
                    } else {
                        printk(LOG_ERROR, "failed to read LBA 1 on nsid %d.\n", nsid);
                    }

                    kfree(buffer);
                }
            }
        }
        if (check_device(i, 0x01, 0x06, 0x01)) {
            init_ahci(devices[i].bus, devices[i].device, devices[i].function);
        }
        if (check_device(i, 0x0C, 0x03, 0x30)) {
            xhci_init_device(devices[i].bus, devices[i].device, devices[i].function);
        }
        if (check_device(i, 0x0C, 0x03, 0x20)) {
            ehci_init_device(devices[i].bus, devices[i].device, devices[i].function);
        }
        printk(LOG_INFO, "PCI DEVICE: %d:%d:%d %x:%x %x:%x\n", devices[i].bus, devices[i].device, devices[i].function, devices[i].class_code, devices[i].subclass, devices[i].device_id, devices[i].vendor_id);
    }
    tests();
    devfs_init();
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
