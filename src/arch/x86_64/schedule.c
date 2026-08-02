// SPDX-License-Identifier: GPL-3.0-only
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <signal.h>
#include <drivers/fb.h>
#include <arch/x86_64/schedule.h>

#define MAX_TASKS 512
#define KERNEL_STACK_SIZE 4096
#define HHDM_OFFSET 0xffff800000000000ULL
#define MAX_MSG_PAYLOAD 256
#define MAX_PROCESS_MSGS 16
#define ERR_IPC_WOULD_BLOCK -2

char cwd[512];
bool defined = false;
typedef uint64_t page_table_t;
extern page_table_t *vmm_get_current_pml4(void);

typedef struct {
    uint32_t sender_pid;
    uint32_t payload_size;
    uint8_t  data[MAX_MSG_PAYLOAD];
} kernel_msg_t;

struct task {
    uint64_t rsp;
    uint64_t user_rsp;
    page_table_t *pml4;
    task_state_t state;
    uint8_t fpu_state[512] __attribute__((aligned(16)));
    uint8_t kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));
    kernel_msg_t msg_queue[MAX_PROCESS_MSGS];
    int msg_head;
    int msg_tail;
    int msg_count;
    int uid;
    int gid;
    int parent_pid;
    uint64_t signal_handlers[32];
    uint32_t pending_signals;
    uint32_t sig_mask;
    char name[32];
    char cwd[512];
};

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

struct task task_table[MAX_TASKS];
volatile int current_task_id = 0;
static const uint32_t default_mxcsr = 0x1f80;
bool running = false;

static void safe_memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static inline bool is_valid_user_pointer(const void *addr, size_t size) {
    uintptr_t uaddr = (uintptr_t)addr;
    if (uaddr == 0) return false;
    if (uaddr >= 0x0000800000000000ULL) return false;
    if (uaddr + size < uaddr || uaddr + size >= 0x0000800000000000ULL) return false;
    return true;
}

static void init_fpu_context(struct task *task) {
    uint32_t mxcsr = default_mxcsr;
    memset((void *)task->fpu_state, 0, sizeof(task->fpu_state));
    asm volatile("fninit" ::: "memory");
    asm volatile("ldmxcsr %0" :: "m"(mxcsr) : "memory");
    asm volatile("fxsave64 %0" : "=m"(task->fpu_state) :: "memory");
}

void fpu_context_save(void) {
    struct task *task = &task_table[current_task_id];
    asm volatile("fxsave64 %0" : "=m"(task->fpu_state) :: "memory");
}

void fpu_context_restore(void) {
    struct task *task = &task_table[current_task_id];
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
    printk(LOG_TRACE, "[SCHED] Kernel thread PID %d exiting\n", current_task_id);
    asm volatile("int $0x30");
    for(;;);
}

void userspace_exit_handler(void) {
    printk(LOG_TRACE, "[SCHED] Userspace task PID %d exiting\n", current_task_id);
    asm volatile("int $0x30");
    for(;;);
}

int create_kernel_task(void (*entry_point)(void), char* name) {
    uint64_t flags = irq_save();

    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].state == TASK_STATE_DEAD) {
            task_table[i].user_rsp = 0;
            task_table[i].msg_head = 0;
            task_table[i].msg_tail = 0;
            task_table[i].msg_count = 0;
            task_table[i].pending_signals = 0;
            task_table[i].sig_mask = 0;
            for (int j = 0; j < 32; j++) {
                task_table[i].signal_handlers[j] = 0;
            }
            init_fpu_context(&task_table[i]);

            uint64_t *kernel_stack_top = (uint64_t *)((uintptr_t)&task_table[i].kernel_stack[KERNEL_STACK_SIZE] & ~0xFULL);

            kernel_stack_top[-1] = (uint64_t)kernel_thread_exit_handler;

            uint64_t *ctx = kernel_stack_top - 24;

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

            ctx[15] = 32; // Vector Index
            ctx[16] = 0;  // Error Code

            ctx[17] = (uint64_t)entry_point;             // RIP
            ctx[18] = 0x08;                              // CS
            ctx[19] = 0x202;                             // RFLAGS
            ctx[20] = (uint64_t)&kernel_stack_top[-1];   // RSP
            ctx[21] = 0x10;                              // SS

            task_table[i].rsp = (uint64_t)ctx;
            task_table[i].pml4 = vmm_get_current_pml4();
            task_table[i].state = TASK_STATE_READY;
            task_table[i].uid = 0;
            strcpy(task_table[i].name, name);
            memset(task_table[i].cwd, 0, 512);
            task_table[i].cwd[0] = '/';
            task_table[i].gid = 0;
            task_table[i].parent_pid = -1;

            printk(LOG_TRACE, "[SCHED] Created kernel task '%s' (PID %d)\n", name, i);

            irq_restore(flags);
            return i;
        }
    }
    printk(LOG_ERROR, "[SCHED_ERR] Failed to create kernel task '%s': task table full\n", name);
    irq_restore(flags);
    return -1;
}

void set_cwd(char* cwdi) {
    if (cwdi != NULL) {
        strcpy(cwd, cwdi);
        defined = true;
    }
}

int create_user_task(void (*entry_point)(void), void* user_stack, uint64_t rdi, uint64_t rsi, void *pml4, int uid, int gid, int pid, char* name) {
    uint64_t flags = irq_save();

    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].state == TASK_STATE_DEAD) {
            init_fpu_context(&task_table[i]);

            task_table[i].user_rsp = ((uint64_t)user_stack) & ~0xFULL;
            task_table[i].pml4 = (page_table_t *)pml4;

            uintptr_t k_stack_raw = (uintptr_t)&task_table[i].kernel_stack[KERNEL_STACK_SIZE];
            uint64_t *kernel_stack_top = (uint64_t *)(k_stack_raw & ~0xFULL);

            uint64_t *ctx = kernel_stack_top - 24;

            ctx[0]  = 0;   // RAX
            ctx[1]  = 0;   // RBX
            ctx[2]  = 0;   // RCX
            ctx[3]  = 0;   // RDX
            ctx[4]  = rsi; // RSI
            ctx[5]  = rdi; // RDI
            ctx[6]  = 0;   // RBP
            ctx[7]  = 0;   // R8
            ctx[8]  = 0;   // R9
            ctx[9]  = 0;   // R10
            ctx[10] = 0;   // R11
            ctx[11] = 0;   // R12
            ctx[12] = 0;   // R13
            ctx[13] = 0;   // R14
            ctx[14] = 0;   // R15

            ctx[15] = 32;  // Vector Index
            ctx[16] = 0;   // Error Code

            ctx[17] = (uint64_t)entry_point;  // RIP
            ctx[18] = 0x1B;                   // CS (User RPL 3)
            ctx[19] = 0x202;                  // RFLAGS
            ctx[20] = task_table[i].user_rsp; // RSP
            ctx[21] = 0x23;                   // SS (User RPL 3)

            memset(task_table[i].cwd, 0, 512);
            if (!defined) {
                task_table[i].cwd[0] = '/';
            } else {
                strcpy(task_table[i].cwd, cwd);
                defined = false;
            }

            int target_idx = i;
            if (pid != -1 && pid >= 0 && pid < MAX_TASKS && task_table[pid].state == TASK_STATE_DEAD) {
                target_idx = pid;
            }

            task_table[target_idx].uid = uid;
            task_table[target_idx].gid = gid;
            task_table[target_idx].pending_signals = 0;
            task_table[target_idx].sig_mask = 0;
            task_table[target_idx].msg_head = 0;
            task_table[target_idx].msg_tail = 0;
            task_table[target_idx].msg_count = 0;
            for (int j = 0; j < 32; j++) {
                task_table[target_idx].signal_handlers[j] = 0;
            }
            task_table[target_idx].rsp = (uint64_t)ctx;
            task_table[target_idx].state = TASK_STATE_READY;
            task_table[target_idx].parent_pid = running ? current_task_id : -1;
            strcpy(task_table[target_idx].name, name);

            printk(LOG_TRACE, "[SCHED] Created user task '%s' (PID %d, Parent %d)\n", name, target_idx, task_table[target_idx].parent_pid);

            irq_restore(flags);
            return target_idx;
        }
    }

    printk(LOG_ERROR, "[SCHED_ERR] Failed to create user task '%s': task table full\n", name);
    irq_restore(flags);
    return -1;
}

static int task_next_pending_signal(struct task *task) {
    for (int sig = 1; sig < 32; sig++) {
        if (task->pending_signals & (1u << sig)) {
            return sig;
        }
    }
    return 0;
}

static bool task_deliver_signal(struct task *task, int sig) {
    if (sig <= 0 || sig >= 32) {
        return false;
    }

    task->pending_signals &= ~(1u << sig);
    uint64_t handler = task->signal_handlers[sig];

    printk(LOG_TRACE, "[SIG] Delivering signal %d to PID %d (handler=0x%lx)\n", sig, current_task_id, handler);

    if (handler == (uint64_t)SIG_IGN) {
        return false;
    }

    if (handler == 0 || handler == (uint64_t)SIG_DFL) {
        if (sig == SIGKILL || sig == SIGTERM) {
            printk(LOG_WARNING, "[SIG] Terminating PID %d via signal %d\n", current_task_id, sig);
            task->state = TASK_STATE_ZOMBIE;
            return true;
        }
        return false;
    }

    uint64_t *ctx = (uint64_t *)task->rsp;
    uint64_t old_rip = ctx[17];
    uint64_t old_rsp = ctx[20];
    uint64_t new_user_rsp = old_rsp - sizeof(uint64_t);
    *(uint64_t *)new_user_rsp = old_rip;
    ctx[20] = new_user_rsp;
    ctx[5] = sig;
    ctx[17] = handler;
    return false;
}

uint64_t set_signal_handler(int sig, uint64_t handler) {
    if (sig <= 0 || sig >= 32) {
        return (uint64_t)SIG_ERR;
    }

    struct task *current = &task_table[current_task_id];
    uint64_t old = current->signal_handlers[sig];
    current->signal_handlers[sig] = handler;
    printk(LOG_TRACE, "[SIG] PID %d set handler for sig %d to 0x%lx\n", current_task_id, sig, handler);
    if (old == 0) {
        return (uint64_t)SIG_DFL;
    }
    return old;
}

int send_signal(int pid, int sig) {
    if (pid < 0 || pid >= MAX_TASKS || sig <= 0 || sig >= 32) {
        return -1;
    }

    struct task *target = &task_table[pid];
    if (target->state == TASK_STATE_DEAD || target->user_rsp == 0) {
        return -1;
    }

    printk(LOG_TRACE, "[SIG] PID %d sending signal %d to PID %d\n", current_task_id, sig, pid);

    if (sig == SIGKILL || sig == SIGTERM) {
        target->state = TASK_STATE_ZOMBIE;
        return 0;
    }

    uint64_t handler = target->signal_handlers[sig];
    if (handler == (uint64_t)SIG_IGN) {
        return 0;
    }

    target->pending_signals |= (1u << sig);
    if (target->state == TASK_STATE_BLOCKED_RECEIVE || target->state == TASK_STATE_WAITING || target->state == TASK_STATE_BLOCKED_SEND) {
        target->state = TASK_STATE_READY;
    }
    return 0;
}

uint64_t schedule_preemptive(uint64_t old_rsp) {
    task_table[current_task_id].rsp = old_rsp;
    int old_task_id = current_task_id;

    if (task_table[old_task_id].state == TASK_STATE_RUNNING) {
        task_table[old_task_id].state = TASK_STATE_READY;
    }

    int next_task_id = -1;
    for (int i = 1; i <= MAX_TASKS; i++) {
        int candidate = (current_task_id + i) % MAX_TASKS;
        if (task_table[candidate].state != TASK_STATE_READY) {
            continue;
        }

        int sig = task_next_pending_signal(&task_table[candidate]);
        if (sig) {
            if (task_deliver_signal(&task_table[candidate], sig)) {
                continue;
            }
        }

        next_task_id = candidate;
        break;
    }

    if (next_task_id == -1) {
        if (task_table[old_task_id].state == TASK_STATE_ZOMBIE) {
            printk(LOG_TRACE, "[SCHED] Reaping last dead task PID %d\n", old_task_id);
            task_table[old_task_id].state = TASK_STATE_DEAD;
            return 0;
        }

        if (task_table[current_task_id].state == TASK_STATE_READY) {
            task_table[current_task_id].state = TASK_STATE_RUNNING;
        }
        return 0;
    }
    current_task_id = next_task_id;
    task_table[current_task_id].state = TASK_STATE_RUNNING;

    if (task_table[current_task_id].pml4) {
        uint64_t new_cr3_phys = (uint64_t)task_table[current_task_id].pml4 - HHDM_OFFSET;
        asm volatile("mov %0, %%cr3" :: "r"(new_cr3_phys) : "memory");
    }

    if (task_table[old_task_id].state == TASK_STATE_ZOMBIE) {
        printk(LOG_TRACE, "[SCHED] Reaping zombie PID %d\n", old_task_id);
        task_table[old_task_id].state = TASK_STATE_DEAD;
        task_table[old_task_id].rsp = 0;
        task_table[old_task_id].user_rsp = 0;
    }

    uint64_t kstack_canonical = (uint64_t)&task_table[current_task_id].kernel_stack[KERNEL_STACK_SIZE];
    global_tss.rsp0 = (uint64_t)(kstack_canonical & ~0xFULL);

    return task_table[current_task_id].rsp;
}

void remove_user_task(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS) return;
    uint64_t flags = irq_save();
    printk(LOG_TRACE, "[SCHED] Removing task PID %d\n", task_id);
    task_table[task_id].state = TASK_STATE_DEAD;
    task_table[task_id].rsp = 0;
    task_table[task_id].user_rsp = 0;
    irq_restore(flags);
}

void init_scheduler(void) {
    printk(LOG_TRACE, "[SCHED] Initializing scheduler table (%d max tasks)\n", MAX_TASKS);
    for (int i = 0; i < MAX_TASKS; i++) {
        task_table[i].state = TASK_STATE_DEAD;
        task_table[i].rsp = 0;
        task_table[i].user_rsp = 0;
        init_fpu_context(&task_table[i]);
    }

    task_table[0].state = TASK_STATE_RUNNING;
    task_table[0].pml4 = vmm_get_current_pml4();
    current_task_id = 0;

    uint64_t kstack_canonical = (uint64_t)&task_table[0].kernel_stack[KERNEL_STACK_SIZE];
    global_tss.rsp0 = (uint64_t)(kstack_canonical & ~0xFULL);
}

void start_scheduler(void) {
    printk(LOG_TRACE, "[SCHED] Starting scheduler loop...\n");
    uint64_t safe_kernel_stack = ((uint64_t)&task_table[0].kernel_stack[KERNEL_STACK_SIZE] & ~0xFULL) - 16;

    asm volatile (
        "mov %0, %%rsp\n\t"
        "mov %%rsp, %%rbp\n\t"
        :
        : "r"(safe_kernel_stack)
        : "memory"
    );
    running = true;
    pit_init();

    asm volatile ("sti; nop" ::: "memory");
    for (;;) {
        asm volatile("hlt");
    }
}

uint64_t syscall_exit_handler(uint64_t current_rsp, uint64_t status) {
    (void)status;
    printk(LOG_TRACE, "[SYSCALL] Exit handler called by PID %d (status %lu)\n", current_task_id, status);
    task_table[current_task_id].state = TASK_STATE_ZOMBIE;

    int parent_pid = task_table[current_task_id].parent_pid;
    if (parent_pid >= 0 && parent_pid < MAX_TASKS) {
        if (task_table[parent_pid].state == TASK_STATE_WAITING) {
            printk(LOG_TRACE, "[SYSCALL] Waking parent PID %d from WAITING state\n", parent_pid);
            task_table[parent_pid].state = TASK_STATE_READY;
        }
    }

    return schedule_preemptive(current_rsp);
}

uint64_t terminate(uint64_t current_rsp, int pid) {
    if (pid < 0 || pid >= MAX_TASKS) return current_rsp;
    printk(LOG_TRACE, "[SYSCALL] Terminate called on PID %d\n", pid);
    task_table[pid].state = TASK_STATE_ZOMBIE;

    int parent_pid = task_table[pid].parent_pid;
    if (parent_pid >= 0 && parent_pid < MAX_TASKS) {
        if (task_table[parent_pid].state == TASK_STATE_WAITING) {
            task_table[parent_pid].state = TASK_STATE_READY;
        }
    }
    return schedule_preemptive(current_rsp);
}

int getpid() {
    return current_task_id;
}

static void ipc_pause(void) {
    asm volatile("int $0x20" ::: "memory");
}

int ipc_send(uint32_t target_pid, const void *buf, uint32_t size) {
    if (target_pid >= MAX_TASKS || size > MAX_MSG_PAYLOAD || target_pid == (uint32_t)current_task_id || buf == NULL) {
        printk(LOG_ERROR, "[IPC_ERR] Invalid send args: src=%d, target=%d, size=%u, buf=%p\n", current_task_id, target_pid, size, buf);
        return -1;
    }
    if (!is_valid_user_pointer(buf, size)) {
        printk(LOG_ERROR, "[IPC_ERR] Invalid user buffer pointer: %p (size %u)\n", buf, size);
        return -1;
    }

    printk(LOG_TRACE, "[IPC] PID %d attempting to send %u bytes to PID %d\n", current_task_id, size, target_pid);

    while (1) {
        uint64_t flags = irq_save();
        struct task *target = &task_table[target_pid];

        if (target->state == TASK_STATE_DEAD || target->state == TASK_STATE_ZOMBIE) {
            printk(LOG_ERROR, "[IPC_ERR] Target PID %d dead or zombie\n", target_pid);
            irq_restore(flags);
            return -1;
        }

        if (target->msg_count < MAX_PROCESS_MSGS) {
            int tail = target->msg_tail;
            kernel_msg_t *msg = &target->msg_queue[tail];

            msg->sender_pid = current_task_id;
            msg->payload_size = size;

            safe_memcpy(msg->data, buf, size);

            target->msg_tail = (tail + 1) % MAX_PROCESS_MSGS;
            target->msg_count++;

            printk(LOG_TRACE, "[IPC] Delivered msg from PID %d to PID %d (queue count %d/%d)\n", current_task_id, target_pid, target->msg_count, MAX_PROCESS_MSGS);

            if (target->state == TASK_STATE_BLOCKED_RECEIVE) {
                printk(LOG_TRACE, "[IPC] Waking target PID %d from BLOCKED_RECEIVE state\n", target_pid);
                target->state = TASK_STATE_READY;
            }

            irq_restore(flags);
            return 0;
        }

        printk(LOG_WARNING, "[IPC] Target PID %d queue full. PID %d blocking on BLOCKED_SEND...\n", target_pid, current_task_id);
        task_table[current_task_id].state = TASK_STATE_BLOCKED_SEND;
        irq_restore(flags);

        ipc_pause();
    }
}

int ipc_recv(void *buf, uint32_t max_size, uint32_t *out_sender_pid) {
    if (buf == NULL || !is_valid_user_pointer(buf, max_size)) {
        printk(LOG_ERROR, "[IPC_ERR] Invalid recv buffer pointer: %p (max_size %u)\n", buf, max_size);
        return -1;
    }

    printk(LOG_TRACE, "[IPC] PID %d attempting to receive msg (max size %u)\n", current_task_id, max_size);

    while (1) {
        uint64_t flags = irq_save();
        struct task *current = &task_table[current_task_id];

        if (current->msg_count > 0) {
            int head = current->msg_head;
            kernel_msg_t *msg = &current->msg_queue[head];

            uint32_t bytes_to_copy = msg->payload_size;
            if (bytes_to_copy > max_size) {
                bytes_to_copy = max_size;
            }

            safe_memcpy(buf, msg->data, bytes_to_copy);
            if (out_sender_pid && is_valid_user_pointer(out_sender_pid, sizeof(uint32_t))) {
                *out_sender_pid = msg->sender_pid;
            }

            current->msg_head = (head + 1) % MAX_PROCESS_MSGS;
            current->msg_count--;

            printk(LOG_TRACE, "[IPC] PID %d consumed msg from sender PID %d (%u bytes)\n", current_task_id, msg->sender_pid, bytes_to_copy);

            for (int i = 0; i < MAX_TASKS; i++) {
                if (task_table[i].state == TASK_STATE_BLOCKED_SEND) {
                    printk(LOG_TRACE, "[IPC] Waking PID %d from BLOCKED_SEND state\n", i);
                    task_table[i].state = TASK_STATE_READY;
                }
            }

            irq_restore(flags);
            return bytes_to_copy;
        }

        printk(LOG_WARNING, "[IPC] Queue empty for PID %d. Blocking on BLOCKED_RECEIVE...\n", current_task_id);
        current->state = TASK_STATE_BLOCKED_RECEIVE;
        irq_restore(flags);

        ipc_pause();
    }
}

int ipc_send_nonblock(uint32_t target_pid, const void *buf, uint32_t size) {
    if (target_pid >= MAX_TASKS || size > MAX_MSG_PAYLOAD || target_pid == (uint32_t)current_task_id || buf == NULL) {
        return -1;
    }
    if (!is_valid_user_pointer(buf, size)) {
        return -1;
    }

    uint64_t flags = irq_save();
    struct task *target = &task_table[target_pid];

    if (target->state == TASK_STATE_DEAD || target->state == TASK_STATE_ZOMBIE) {
        irq_restore(flags);
        return -1;
    }

    if (target->msg_count >= MAX_PROCESS_MSGS) {
        printk(LOG_WARNING, "[IPC] Non-blocking send from PID %d to PID %d would block\n", current_task_id, target_pid);
        irq_restore(flags);
        return ERR_IPC_WOULD_BLOCK;
    }

    int tail = target->msg_tail;
    kernel_msg_t *msg = &target->msg_queue[tail];

    msg->sender_pid = current_task_id;
    msg->payload_size = size;
    safe_memcpy(msg->data, buf, size);

    target->msg_tail = (tail + 1) % MAX_PROCESS_MSGS;
    target->msg_count++;

    printk(LOG_TRACE, "[IPC] Non-block delivered msg from PID %d to PID %d\n", current_task_id, target_pid);

    if (target->state == TASK_STATE_BLOCKED_RECEIVE) {
        target->state = TASK_STATE_READY;
    }

    irq_restore(flags);
    return 0;
}

int ipc_recv_nonblock(void *buf, uint32_t max_size, uint32_t *out_sender_pid) {
    if (buf == NULL || !is_valid_user_pointer(buf, max_size)) {
        return -1;
    }

    uint64_t flags = irq_save();
    struct task *current = &task_table[current_task_id];

    if (current->msg_count == 0) {
        irq_restore(flags);
        return ERR_IPC_WOULD_BLOCK;
    }

    int head = current->msg_head;
    kernel_msg_t *msg = &current->msg_queue[head];

    uint32_t bytes_to_copy = msg->payload_size;
    if (bytes_to_copy > max_size) {
        bytes_to_copy = max_size;
    }

    safe_memcpy(buf, msg->data, bytes_to_copy);
    if (out_sender_pid && is_valid_user_pointer(out_sender_pid, sizeof(uint32_t))) {
        *out_sender_pid = msg->sender_pid;
    }

    current->msg_head = (head + 1) % MAX_PROCESS_MSGS;
    current->msg_count--;

    printk(LOG_TRACE, "[IPC] Non-block consumed msg for PID %d\n", current_task_id);

    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].state == TASK_STATE_BLOCKED_SEND) {
            task_table[i].state = TASK_STATE_READY;
        }
    }

    irq_restore(flags);
    return bytes_to_copy;
}

int getuid() {
    return task_table[current_task_id].uid;
}

int getgid() {
    return task_table[current_task_id].gid;
}

int waitpid(uint64_t pid) {
    if (pid >= MAX_TASKS || !task_table[pid].user_rsp)
        return -1;

    printk(LOG_TRACE, "[SCHED] PID %d waitpid waiting on PID %lu\n", current_task_id, pid);

    while (task_table[pid].state != TASK_STATE_ZOMBIE && task_table[pid].state != TASK_STATE_DEAD) {
        uint64_t flags = irq_save();
        task_table[current_task_id].state = TASK_STATE_WAITING;
        irq_restore(flags);

        ipc_pause();
    }

    task_table[current_task_id].state = TASK_STATE_READY;
    printk(LOG_TRACE, "[SCHED] PID %d finished waitpid on PID %lu\n", current_task_id, pid);
    return 0;
}

char* getpcwd() {
    return task_table[current_task_id].cwd;
}

int ps(struct utask* u, int max_len) {
    int len_found = 0;

    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].state == TASK_STATE_DEAD)
            continue;

        if (len_found >= max_len)
            return -1;

        strcpy(u[len_found].cwd, task_table[i].cwd);
        strcpy(u[len_found].name, task_table[i].name);

        u[len_found].gid = task_table[i].gid;
        u[len_found].uid = task_table[i].uid;
        u[len_found].pid = i;
        u[len_found].parent_pid = task_table[i].parent_pid;
        u[len_found].state = task_table[i].state;

        len_found++;
    }

    return len_found;
}