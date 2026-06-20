// SPDX-License-Identifier: GPL-3.0-only
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <drivers/fb.h>
#define MAX_TASKS 16
#define KERNEL_STACK_SIZE 4096
#define USER_STACK_SIZE 4096
#define HHDM_OFFSET 0xffff800000000000ULL
#define MAX_MSG_PAYLOAD 128  // Maximum bytes per single message
#define MAX_PROCESS_MSGS 16  // Maximum pending messages a process can hold

// The message structure stored in the kernel
typedef struct {
    uint32_t sender_pid;
    uint32_t payload_size;
    uint8_t  data[MAX_MSG_PAYLOAD];
} kernel_msg_t;
typedef enum {
    TASK_STATE_DEAD = 0,
    TASK_STATE_READY,
    TASK_STATE_RUNNING,
    TASK_STATE_ZOMBIE,
    TASK_STATE_BLOCKED_SEND,    // Added: Waiting for space in a target queue
    TASK_STATE_BLOCKED_RECEIVE
} task_state_t;

// Task Control Block (TCB)
struct task {
    uint64_t rsp;               // Saved kernel stack pointer 
    uint64_t user_rsp;          // Saved userspace stack pointer (0 for kthreads)
    task_state_t state;         
    uint8_t fpu_state[512] __attribute__((aligned(16)));
    uint8_t kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16))); 
    kernel_msg_t msg_queue[MAX_PROCESS_MSGS];
    int msg_head;
    int msg_tail;
    int msg_count;
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
static const uint32_t default_mxcsr = 0x1f80;

static void init_fpu_context(struct task *task) {
    uint32_t mxcsr = default_mxcsr;

    memset((void *)task->fpu_state, 0, sizeof(task->fpu_state));
    asm volatile("fninit" ::: "memory");
    asm volatile("ldmxcsr %0" :: "m"(mxcsr) : "memory");
    asm volatile("fxsave64 %0" : "=m"(task->fpu_state) :: "memory");
}

void fpu_context_save(void) {
    struct task *task = (struct task *)&task_table[current_task_id];
    asm volatile("fxsave64 %0" : "=m"(task->fpu_state) :: "memory");
}

void fpu_context_restore(void) {
    struct task *task = (struct task *)&task_table[current_task_id];
    asm volatile("fxrstor64 %0" :: "m"(task->fpu_state) : "memory");
}

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
            task_table[i].msg_head = 0;
            task_table[i].msg_tail = 0;
            task_table[i].msg_count = 0;
            init_fpu_context((struct task *)&task_table[i]);

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
            init_fpu_context((struct task *)&task_table[i]);
            
            uint64_t user_stack_vma_top = 0x600000 + USER_STACK_SIZE;
            uint64_t *user_virt_stack_top = (uint64_t *)(user_stack_vma_top & ~0xFULL);

            uint64_t raw_phys_stack = (uint64_t)allocated_user_stack;
            if (raw_phys_stack >= HHDM_OFFSET) {
                raw_phys_stack -= HHDM_OFFSET;
            }

            uint64_t *user_stack_hhdm_top = (uint64_t *)(raw_phys_stack + USER_STACK_SIZE + HHDM_OFFSET);
            user_stack_hhdm_top = (uint64_t *)((uintptr_t)user_stack_hhdm_top & ~0xFULL);

            /*
             * The user image starts in crt0.asm, which reads argc/argv from the
             * initial stack and then CALLs main(). That means _start must enter
             * with a 16-byte aligned RSP, so main itself lands on the standard
             * SysV ABI boundary.
             */
            user_stack_hhdm_top[-2] = 0; // argc
            user_stack_hhdm_top[-1] = 0; // argv
            task_table[i].user_rsp = (uint64_t)&user_virt_stack_top[-2];

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

uint64_t schedule_preemptive(uint64_t old_rsp) {
    // Save the old stack pointer to its slot
    task_table[current_task_id].rsp = old_rsp;
    
    // Remember the task that just ran
    int old_task_id = current_task_id;

    if (task_table[old_task_id].state == TASK_STATE_RUNNING) {
        task_table[old_task_id].state = TASK_STATE_READY;
    }

    // Find the next available ready task
    int next_task_id = -1;
    for (int i = 1; i <= MAX_TASKS; i++) {
        int candidate = (current_task_id + i) % MAX_TASKS;
        if (task_table[candidate].state == TASK_STATE_READY) {
            next_task_id = candidate;
            break;
        }
    }

    // If no other runnable threads exist
    if (next_task_id == -1) {
        // If the current task died (is a zombie) and nothing else can run, the system must halt
        if (task_table[old_task_id].state == TASK_STATE_ZOMBIE) {
            // Free the last task's table tracking slot safely
            task_table[old_task_id].state = TASK_STATE_DEAD;
            return 0; // Returning 0 will fall back to your assembly idle/halt loop
        }
        
        if (task_table[current_task_id].state == TASK_STATE_READY) {
            task_table[current_task_id].state = TASK_STATE_RUNNING;
        }
        return 0; 
    }

    // Switch contexts to the new task
    current_task_id = next_task_id;
    task_table[current_task_id].state = TASK_STATE_RUNNING;

    // --- ADDED CLEANUP MECHANISM ---
    // If the previous task exited and became a zombie, safely free its slot now
    if (task_table[old_task_id].state == TASK_STATE_ZOMBIE) {
        task_table[old_task_id].state = TASK_STATE_DEAD;
        task_table[old_task_id].rsp = 0;
        task_table[old_task_id].user_rsp = 0;
        // NOTE: If you implemented virtual memory page directories (cr3) later,
        // you would also call your memory manager to free the process memory pages here.
    }

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
        init_fpu_context((struct task *)&task_table[i]);
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

// Update your function signature to accept the status from RSI
uint64_t syscall_exit_handler(uint64_t current_rsp, uint64_t status) {
    
    // Log the exit status inside the kernel console for debugging
    printk(LOG_INFO, "Task %d exited with status: %d\n", current_task_id, (int)status);
    
    // Mark the task as a zombie so the scheduler cleans it up
    task_table[current_task_id].state = TASK_STATE_ZOMBIE;
    
    // Force a context switch to the next ready task
    return schedule_preemptive(current_rsp);
}
int getpid() {
    return current_task_id;
}/**
 * BLOCKING SEND
 * If the target queue is full, this task sleeps until space is available.
 */
int ipc_send(uint32_t target_pid, const void *buf, uint32_t size) {
    if (target_pid >= MAX_TASKS || size > MAX_MSG_PAYLOAD || target_pid == current_task_id) {
        return -1;
    }

    while (1) {
        uint64_t flags = irq_save();
        volatile struct task *target = &task_table[target_pid];

        // If target died while we were waiting, abort
        if (target->state == TASK_STATE_DEAD || target->state == TASK_STATE_ZOMBIE) {
            irq_restore(flags);
            return -1;
        }

        // If there is room in the target's queue, perform the write
        if (target->msg_count < MAX_PROCESS_MSGS) {
            int tail = target->msg_tail;
            kernel_msg_t *msg = (kernel_msg_t *)&target->msg_queue[tail];
            
            msg->sender_pid = current_task_id;
            msg->payload_size = size;
            memcpy(msg->data, buf, size);

            target->msg_tail = (tail + 1) % MAX_PROCESS_MSGS;
            target->msg_count++;

            // WAKE UP TARGET: If the target was blocked waiting for a message, make it ready
            if (target->state == TASK_STATE_BLOCKED_RECEIVE) {
                target->state = TASK_STATE_READY;
            }

            irq_restore(flags);
            return 0; // Success
        }

        // QUEUE FULL: Block current task and yield CPU
        task_table[current_task_id].state = TASK_STATE_BLOCKED_SEND;
        
        // Pass the current stack pointer to the scheduler so it can save our place.
        // We will wake up exactly right here when someone reads from the target queue.
        uint64_t current_rsp;
        asm volatile("mov %%rsp, %0" : "=r"(current_rsp));
        
        irq_restore(flags);
        schedule_preemptive(current_rsp); 
    }
}

/**
 * BLOCKING RECEIVE
 * If the queue is empty, this task sleeps until a message arrives.
 */
int ipc_recv(void *buf, uint32_t max_size, uint32_t *out_sender_pid) {
    while (1) {
        uint64_t flags = irq_save();
        volatile struct task *current = &task_table[current_task_id];

        // If there is a message, consume it
        if (current->msg_count > 0) {
            int head = current->msg_head;
            kernel_msg_t *msg = (kernel_msg_t *)&current->msg_queue[head];

            uint32_t bytes_to_copy = msg->payload_size;
            if (bytes_to_copy > max_size) {
                bytes_to_copy = max_size;
            }

            memcpy(buf, msg->data, bytes_to_copy);
            if (out_sender_pid) {
                *out_sender_pid = msg->sender_pid;
            }

            current->msg_head = (head + 1) % MAX_PROCESS_MSGS;
            current->msg_count--;

            // WAKE UP SENDERS: Check if any task was blocked trying to send to us
            for (int i = 0; i < MAX_TASKS; i++) {
                // If a task is blocked trying to send to this exact process, wake it up
                if (task_table[i].state == TASK_STATE_BLOCKED_SEND) {
                    // Note: In a complete implementation, you'd check if 'i' was targeting 'current_task_id'
                    // For a simple loop, waking up any blocked sender to re-check their target is safe.
                    task_table[i].state = TASK_STATE_READY;
                }
            }

            irq_restore(flags);
            return bytes_to_copy;
        }

        // QUEUE EMPTY: Block current task and yield CPU
        current->state = TASK_STATE_BLOCKED_RECEIVE;

        uint64_t current_rsp;
        asm volatile("mov %%rsp, %0" : "=r"(current_rsp));

        irq_restore(flags);
        schedule_preemptive(current_rsp);
    }
}