// SPDX-License-Identifier: GPL-3.0-only
#include <stdint.h>
#include <stddef.h>

#define MAX_TASKS 16
#define KERNEL_STACK_SIZE 4096
#define USER_STACK_SIZE 4096
#define HHDM_OFFSET 0xffff800000000000ULL

typedef enum {
    TASK_STATE_DEAD = 0,
    TASK_STATE_READY,
    TASK_STATE_RUNNING,
    TASK_STATE_ZOMBIE
} task_state_t;

// Task Control Block (TCB)
struct task {
    uint64_t rsp;               // Saved kernel stack pointer 
    uint64_t user_rsp;          // Saved userspace stack pointer (0 for kthreads)
    task_state_t state;         
    uint8_t kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16))); 
};

// Task State Segment (TSS) layout for x86_64
struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1; uint64_t ist2; uint64_t ist3; uint64_t ist4; uint64_t ist5; uint64_t ist6; uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

extern struct tss_entry global_tss;
extern void pit_init(void); 

// Global volatile scheduler tracking structures
volatile struct task task_table[MAX_TASKS];
volatile int current_task_id = 0;

static uint64_t irq_save(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) {
        asm volatile("sti" ::: "memory");
    }
}

void kernel_thread_exit_handler(void) {
    asm volatile("int $0x30"); 
    for(;;);
}

void userspace_exit_handler(void) {
    asm volatile("int $0x30"); 
    for(;;);
}

/**
 * CREATE A KERNEL THREAD (Ring 0)
 */
int create_kernel_task(void (*entry_point)(void)) {
    uint64_t flags = irq_save();

    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].state == TASK_STATE_DEAD) {
            task_table[i].user_rsp = 0;

            uint64_t *kernel_stack_top = (uint64_t *)((uintptr_t)&task_table[i].kernel_stack[KERNEL_STACK_SIZE] & ~0xFULL);

            // Write the cleanup handler address if the thread routine returns
            kernel_stack_top[-1] = (uint64_t)kernel_thread_exit_handler;

            // Direct mapping layout to match the assembly context frames precisely
            uint64_t *ctx = kernel_stack_top - 24;

            // General Purpose Registers (0-14)
            ctx[0]  = 0; // RAX
            ctx[1]  = 0; // RBX
            ctx[2]  = 0; // RCX
            ctx[3]  = 0; // RDX
            ctx[4]  = 0; // RSI
            ctx[5]  = 0; // RDI
            ctx[6]  = 0; // RBP
            ctx[7]  = 0; // R8
            ctx[8]  = 0; // R9
            ctx[9]  = 0; // R10
            ctx[10] = 0; // R11
            ctx[11] = 0; // R12
            ctx[12] = 0; // R13
            ctx[13] = 0; // R14
            ctx[14] = 0; // R15

            // Stack Frame Stubs (15-16)
            ctx[15] = 32; // Vector Index (defaulting to timer slot)
            ctx[16] = 0;  // Error Code

            // CPU Architectural IRETQ Frame (17-21)
            ctx[17] = (uint64_t)entry_point;             // RIP
            ctx[18] = 0x08;                              // CS: Kernel Code Selector (Ring 0)
            ctx[19] = 0x202;                             // RFLAGS: Interrupts Enabled
            ctx[20] = (uint64_t)&kernel_stack_top[-1];   // RSP
            ctx[21] = 0x10;                              // SS: Kernel Data Selector (Ring 0)

            task_table[i].rsp = (uint64_t)ctx;
            task_table[i].state = TASK_STATE_READY;
            
            irq_restore(flags);
            return i;
        }
    }
    irq_restore(flags);
    return -1;
}

/**
 * CREATE A USERSPACE TASK (Ring 3)
 */
int create_user_task(void (*entry_point)(void), void* allocated_user_stack) {
    uint64_t flags = irq_save();

    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].state == TASK_STATE_DEAD) {
            
            uint64_t user_stack_vma_top = 0x600000 + USER_STACK_SIZE;
            uint64_t *user_virt_stack_top = (uint64_t *)(user_stack_vma_top & ~0xFULL);

            uint64_t raw_phys_stack = (uint64_t)allocated_user_stack;
            if (raw_phys_stack >= HHDM_OFFSET) {
                raw_phys_stack -= HHDM_OFFSET;
            }

            uint64_t *kernel_hhdm_stack_top = (uint64_t *)(raw_phys_stack + USER_STACK_SIZE + HHDM_OFFSET);
            kernel_hhdm_stack_top = (uint64_t *)((uintptr_t)kernel_hhdm_stack_top & ~0xFULL);

            kernel_hhdm_stack_top[-1] = (uint64_t)userspace_exit_handler;
            task_table[i].user_rsp = (uint64_t)&user_virt_stack_top[-1];

            uintptr_t k_stack_raw = (uintptr_t)&task_table[i].kernel_stack[KERNEL_STACK_SIZE];
            uint64_t *kernel_stack_top = (uint64_t *)(k_stack_raw & ~0xFULL); 

            uint64_t *ctx = kernel_stack_top - 24;

            // General Purpose Registers (0-14)
            ctx[0]  = 0; // RAX
            ctx[1]  = 0; // RBX
            ctx[2]  = 0; // RCX
            ctx[3]  = 0; // RDX
            ctx[4]  = 0; // RSI
            ctx[5]  = 0; // RDI
            ctx[6]  = 0; // RBP
            ctx[7]  = 0; // R8
            ctx[8]  = 0; // R9
            ctx[9]  = 0; // R10
            ctx[10] = 0; // R11
            ctx[11] = 0; // R12
            ctx[12] = 0; // R13
            ctx[13] = 0; // R14
            ctx[14] = 0; // R15

            // Stack Frame Stubs (15-16)
            ctx[15] = 32; // Vector Index
            ctx[16] = 0;  // Error Code

            // CPU Architectural IRETQ Frame (17-21)
            ctx[17] = (uint64_t)entry_point;  // RIP
            ctx[18] = 0x1B;                   // CS: User Code Selector (RPL 3)
            ctx[19] = 0x202;                  // RFLAGS: Interrupts Enabled
            ctx[20] = task_table[i].user_rsp; // RSP
            ctx[21] = 0x23;                   // SS: User Data Selector (RPL 3)

            task_table[i].rsp = (uint64_t)ctx;
            task_table[i].state = TASK_STATE_READY;
            
            irq_restore(flags);
            return i;
        }
    }
    
    irq_restore(flags);
    return -1;
}

/**
 * CORE PREEMPTIVE ROUND-ROBIN SCHEDULER
 */
uint64_t schedule_preemptive(uint64_t old_rsp) {
    task_table[current_task_id].rsp = old_rsp;
    
    if (task_table[current_task_id].state == TASK_STATE_RUNNING) {
        task_table[current_task_id].state = TASK_STATE_READY;
    }

    int next_task_id = -1;
    for (int i = 1; i <= MAX_TASKS; i++) {
        int candidate = (current_task_id + i) % MAX_TASKS;
        if (task_table[candidate].state == TASK_STATE_READY) {
            next_task_id = candidate;
            break;
        }
    }

    // If no other runable threads exist, yield back to current thread context
    if (next_task_id == -1) {
        if (task_table[current_task_id].state == TASK_STATE_READY) {
            task_table[current_task_id].state = TASK_STATE_RUNNING;
        }
        return 0; 
    }

    current_task_id = next_task_id;
    task_table[current_task_id].state = TASK_STATE_RUNNING;

    // Update TSS so the CPU switches to the right kernel stack during future user interrupts
    uint64_t kstack_canonical = (uint64_t)&task_table[current_task_id].kernel_stack[KERNEL_STACK_SIZE];
    global_tss.rsp0 = (uint64_t)(kstack_canonical & ~0xFULL);

    return task_table[current_task_id].rsp;
}

void remove_user_task(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS) return;
    uint64_t flags = irq_save();
    task_table[task_id].state = TASK_STATE_DEAD;
    task_table[task_id].rsp = 0;
    task_table[task_id].user_rsp = 0;
    irq_restore(flags);
}

void init_scheduler(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        task_table[i].state = TASK_STATE_DEAD;
        task_table[i].rsp = 0;
        task_table[i].user_rsp = 0;
    }

    task_table[0].state = TASK_STATE_RUNNING;
    current_task_id = 0;

    uint64_t kstack_canonical = (uint64_t)&task_table[0].kernel_stack[KERNEL_STACK_SIZE];
    global_tss.rsp0 = (uint64_t)(kstack_canonical & ~0xFULL);
}

void start_scheduler(void) {
    uint64_t safe_kernel_stack = ((uint64_t)&task_table[0].kernel_stack[KERNEL_STACK_SIZE] & ~0xFULL) - 16;

    asm volatile (
        "mov %0, %%rsp\n\t"
        "mov %%rsp, %%rbp\n\t"
        :
        : "r"(safe_kernel_stack)
        : "memory"
    );

    pit_init();
    asm volatile ("sti; nop" ::: "memory");
    
    for (;;) {
        asm volatile("hlt");
    }
}

uint64_t syscall_exit_handler(uint64_t current_rsp) {
    task_table[current_task_id].state = TASK_STATE_ZOMBIE;
    return schedule_preemptive(current_rsp);
}