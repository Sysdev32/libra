// SPDX-License-Identifier: GPL-3.0-only
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <signal.h>
#include <drivers/fb.h>
#include <arch/x86_64/schedule.h>
char cwd[512];
bool defined = false;
typedef uint64_t page_table_t;
extern page_table_t *vmm_get_current_pml4(void);


struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

extern struct tss_entry global_tss;
extern void pit_init(void);

struct process process_table[MAX_PROCESSES];
struct thread thread_table[MAX_THREADS];

volatile int current_thread_id = 0;
static const uint32_t default_mxcsr = 0x1f80;
bool running = false;

/* Hardware MSR Operations */
static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high) : "memory");
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low;
    uint32_t high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void write_fs_base(uint64_t val) {
    wrmsr(IA32_FS_BASE, val);
}

static inline uint64_t read_fs_base(void) {
    return rdmsr(IA32_FS_BASE);
}

static void safe_memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static inline bool is_valid_user_pointer(const void *addr, size_t size) {
    uintptr_t uaddr = (uintptr_t)addr;
    if (uaddr == 0) {
        return false;
    }
    if (uaddr >= 0x0000800000000000ULL) {
        return false;
    }
    if (uaddr + size < uaddr || uaddr + size >= 0x0000800000000000ULL) {
        return false;
    }
    return true;
}

static void init_fpu_context(struct thread *thread) {
    uint32_t mxcsr = default_mxcsr;
    memset((void *)thread->fpu_state, 0, sizeof(thread->fpu_state));
    asm volatile("fninit" ::: "memory");
    asm volatile("ldmxcsr %0" :: "m"(mxcsr) : "memory");
    asm volatile("fxsave64 %0" : "=m"(thread->fpu_state) :: "memory");
}

void fpu_context_save(void) {
    struct thread *thread = &thread_table[current_thread_id];
    asm volatile("fxsave64 %0" : "=m"(thread->fpu_state) :: "memory");
}

void fpu_context_restore(void) {
    struct thread *thread = &thread_table[current_thread_id];
    asm volatile("fxrstor64 %0" :: "m"(thread->fpu_state) : "memory");
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
    printk(LOG_TRACE, "[SCHED] Kernel thread TID %d exiting\n", current_thread_id);
    asm volatile("int $0x30");
    for(;;);
}

void userspace_exit_handler(void) {
    printk(LOG_TRACE, "[SCHED] Userspace thread TID %d exiting\n", current_thread_id);
    asm volatile("int $0x30");
    for(;;);
}

struct process *create_process(const char *name, page_table_t *pml4, int uid, int gid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!process_table[i].active) {
            process_table[i].pid = i;
            if (pml4 != NULL) {
                process_table[i].pml4 = pml4;
            } else {
                process_table[i].pml4 = vmm_get_current_pml4();
            }
            process_table[i].uid = uid;
            process_table[i].gid = gid;

            if (running && thread_table[current_thread_id].process != NULL) {
                process_table[i].parent_pid = thread_table[current_thread_id].process->pid;
            } else {
                process_table[i].parent_pid = -1;
            }

            process_table[i].msg_head = 0;
            process_table[i].msg_tail = 0;
            process_table[i].msg_count = 0;
            process_table[i].pending_signals = 0;
            process_table[i].sig_mask = 0;

            for (int j = 0; j < 32; j++) {
                process_table[i].signal_handlers[j] = 0;
            }

            memset(process_table[i].cwd, 0, 512);
            if (!defined) {
                process_table[i].cwd[0] = '/';
            } else {
                strcpy(process_table[i].cwd, cwd);
                defined = false;
            }

            strcpy(process_table[i].name, name);
            process_table[i].active = true;
            return &process_table[i];
        }
    }
    return NULL;
}

int create_thread(struct process *proc, void (*entry_point)(void), void *user_stack, uint64_t rdi, uint64_t rsi, uint64_t fs_base, bool is_user) {
    uint64_t flags = irq_save();

    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state == TASK_STATE_DEAD) {
            init_fpu_context(&thread_table[i]);

            thread_table[i].tid = i;
            thread_table[i].process = proc;
            thread_table[i].gs_base = 0;

            // Initialize embedded static TCB if no TLS base pointer is provided
            if (fs_base == 0) {
                thread_table[i].tcb.self = &thread_table[i].tcb;
                thread_table[i].fs_base = (uint64_t)&thread_table[i].tcb;
            } else {
                thread_table[i].fs_base = fs_base;
            }

            if (is_user) {
                thread_table[i].user_rsp = ((uint64_t)user_stack) & ~0xFULL;
            } else {
                thread_table[i].user_rsp = 0;
            }

            uintptr_t k_stack_raw = (uintptr_t)&thread_table[i].kernel_stack[KERNEL_STACK_SIZE];
            uint64_t *kernel_stack_top = (uint64_t *)(k_stack_raw & ~0xFULL);

            if (!is_user) {
                kernel_stack_top[-1] = (uint64_t)kernel_thread_exit_handler;
            }

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

            ctx[17] = (uint64_t)entry_point;                     // RIP
            ctx[18] = is_user ? 0x1B : 0x08;                     // CS
            ctx[19] = 0x202;                                     // RFLAGS
            ctx[20] = is_user ? thread_table[i].user_rsp : (uint64_t)&kernel_stack_top[-1]; // RSP
            ctx[21] = is_user ? 0x23 : 0x10;                     // SS

            thread_table[i].rsp = (uint64_t)ctx;
            thread_table[i].state = TASK_STATE_READY;
            memset(thread_table[i].tls_slots, 0, sizeof(thread_table[i].tls_slots));

            printk(LOG_TRACE, "[SCHED] Created %s thread TID %d (PID %d, FS_BASE=0x%lx)\n",
                   is_user ? "user" : "kernel", i, proc->pid, thread_table[i].fs_base);

            irq_restore(flags);
            return i;
        }
    }

    printk(LOG_ERROR, "[SCHED_ERR] Failed to create thread: thread table full\n");
    irq_restore(flags);
    return -1;
}
int create_kernel_task(void (*entry_point)(void), char* name) {
    struct process *proc = create_process(name, vmm_get_current_pml4(), 0, 0);
    if (proc == NULL) {
        printk(LOG_ERROR, "[SCHED_ERR] Failed to create process for kernel task '%s'\n", name);
        return -1;
    }
    return create_thread(proc, entry_point, NULL, 0, 0, 0, false);
}

void set_cwd(char* cwdi) {
    if (cwdi != NULL) {
        strcpy(cwd, cwdi);
        defined = true;
    }
}

int create_user_task(void (*entry_point)(void), void* user_stack, uint64_t rdi, uint64_t rsi, void *pml4, int uid, int gid, int pid, char* name) {
    struct process *proc = NULL;
    if (pid != -1 && pid >= 0 && pid < MAX_PROCESSES && !process_table[pid].active) {
        process_table[pid].pid = pid;
        process_table[pid].pml4 = (page_table_t*)pml4;
        process_table[pid].uid = uid;
        process_table[pid].gid = gid;
        process_table[pid].parent_pid = running ? thread_table[current_thread_id].process->pid : -1;
        process_table[pid].msg_head = 0;
        process_table[pid].msg_tail = 0;
        process_table[pid].msg_count = 0;
        process_table[pid].pending_signals = 0;
        process_table[pid].sig_mask = 0;
        for (int j = 0; j < 32; j++) {
            process_table[pid].signal_handlers[j] = 0;
        }
        memset(process_table[pid].cwd, 0, 512);
        if (!defined) {
            process_table[pid].cwd[0] = '/';
        } else {
            strcpy(process_table[pid].cwd, cwd);
            defined = false;
        }
        strcpy(process_table[pid].name, name);
        process_table[pid].active = true;
        proc = &process_table[pid];
    } else {
        proc = create_process(name, (page_table_t*)pml4, uid, gid);
    }

    if (proc == NULL) {
        printk(LOG_ERROR, "[SCHED_ERR] Failed to create process for user task '%s'\n", name);
        return -1;
    }

    int tid = create_thread(proc, entry_point, user_stack, rdi, rsi, 0, true);
    if (tid >= 0) {
        printk(LOG_TRACE, "[SCHED] Created user task '%s' (PID %d, Parent %d)\n", name, proc->pid, proc->parent_pid);
    }
    return tid;
}

void sys_set_fs_base(uint64_t base) {
    uint64_t flags = irq_save();
    thread_table[current_thread_id].fs_base = base;
    write_fs_base(base);
    irq_restore(flags);
}

uint64_t sys_get_fs_base(void) {
    return thread_table[current_thread_id].fs_base;
}

static int process_next_pending_signal(struct process *proc) {
    for (int sig = 1; sig < 32; sig++) {
        if (proc->pending_signals & (1u << sig)) {
            return sig;
        }
    }
    return 0;
}

static bool thread_deliver_signal(struct thread *thread, int sig) {
    if (sig <= 0 || sig >= 32) {
        return false;
    }

    struct process *proc = thread->process;
    proc->pending_signals &= ~(1u << sig);
    uint64_t handler = proc->signal_handlers[sig];

    printk(LOG_TRACE, "[SIG] Delivering signal %d to TID %d (PID %d, handler=0x%lx)\n",
           sig, thread->tid, proc->pid, handler);

    if (handler == (uint64_t)SIG_IGN) {
        return false;
    }

    if (handler == 0 || handler == (uint64_t)SIG_DFL) {
        if (sig == SIGKILL || sig == SIGTERM) {
            printk(LOG_WARNING, "[SIG] Terminating TID %d via signal %d\n", thread->tid, sig);
            thread->state = TASK_STATE_ZOMBIE;
            return true;
        }
        return false;
    }

    uint64_t *ctx = (uint64_t *)thread->rsp;
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

    struct process *proc = thread_table[current_thread_id].process;
    uint64_t old = proc->signal_handlers[sig];
    proc->signal_handlers[sig] = handler;
    printk(LOG_TRACE, "[SIG] PID %d set handler for sig %d to 0x%lx\n", proc->pid, sig, handler);
    if (old == 0) {
        return (uint64_t)SIG_DFL;
    }
    return old;
}

int send_signal(int pid, int sig) {
    if (pid < 0 || pid >= MAX_PROCESSES || sig <= 0 || sig >= 32) {
        return -1;
    }

    struct process *target = &process_table[pid];
    if (!target->active) {
        return -1;
    }

    printk(LOG_TRACE, "[SIG] PID %d sending signal %d to PID %d\n",
           thread_table[current_thread_id].process->pid, sig, pid);

    if (sig == SIGKILL || sig == SIGTERM) {
        for (int i = 0; i < MAX_THREADS; i++) {
            if (thread_table[i].process == target) {
                thread_table[i].state = TASK_STATE_ZOMBIE;
            }
        }
        return 0;
    }

    uint64_t handler = target->signal_handlers[sig];
    if (handler == (uint64_t)SIG_IGN) {
        return 0;
    }

    target->pending_signals |= (1u << sig);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].process == target) {
            if (thread_table[i].state == TASK_STATE_BLOCKED_RECEIVE ||
                thread_table[i].state == TASK_STATE_WAITING ||
                thread_table[i].state == TASK_STATE_BLOCKED_SEND) {
                thread_table[i].state = TASK_STATE_READY;
            }
        }
    }
    return 0;
}

uint64_t schedule_preemptive(uint64_t old_rsp) {
    thread_table[current_thread_id].rsp = old_rsp;
    int old_thread_id = current_thread_id;

    if (thread_table[old_thread_id].state == TASK_STATE_RUNNING) {
        thread_table[old_thread_id].state = TASK_STATE_READY;
    }

    int next_thread_id = -1;
    for (int i = 1; i <= MAX_THREADS; i++) {
        int candidate = (current_thread_id + i) % MAX_THREADS;
        if (thread_table[candidate].state != TASK_STATE_READY) {
            continue;
        }

        struct process *proc = thread_table[candidate].process;
        int sig = process_next_pending_signal(proc);
        if (sig) {
            if (thread_deliver_signal(&thread_table[candidate], sig)) {
                continue;
            }
        }

        next_thread_id = candidate;
        break;
    }

    if (next_thread_id == -1) {
        if (thread_table[old_thread_id].state == TASK_STATE_ZOMBIE) {
            printk(LOG_TRACE, "[SCHED] Reaping last dead thread TID %d\n", old_thread_id);
            thread_table[old_thread_id].state = TASK_STATE_DEAD;
            return 0;
        }

        if (thread_table[current_thread_id].state == TASK_STATE_READY) {
            thread_table[current_thread_id].state = TASK_STATE_RUNNING;
        }
        return 0;
    }

    current_thread_id = next_thread_id;
    struct thread *curr_thread = &thread_table[current_thread_id];
    curr_thread->state = TASK_STATE_RUNNING;

    if (curr_thread->process && curr_thread->process->pml4) {
        uint64_t new_cr3_phys = (uint64_t)curr_thread->process->pml4 - HHDM_OFFSET;
        asm volatile("mov %0, %%cr3" :: "r"(new_cr3_phys) : "memory");
    }

    /* Restore Thread-Local Storage base register */
    write_fs_base(curr_thread->fs_base);

    if (thread_table[old_thread_id].state == TASK_STATE_ZOMBIE) {
        printk(LOG_TRACE, "[SCHED] Reaping zombie thread TID %d\n", old_thread_id);
        thread_table[old_thread_id].state = TASK_STATE_DEAD;
        thread_table[old_thread_id].rsp = 0;
        thread_table[old_thread_id].user_rsp = 0;
    }

    uint64_t kstack_canonical = (uint64_t)&curr_thread->kernel_stack[KERNEL_STACK_SIZE];
    global_tss.rsp0 = (uint64_t)(kstack_canonical & ~0xFULL);

    return curr_thread->rsp;
}

void remove_user_task(int thread_id) {
    if (thread_id < 0 || thread_id >= MAX_THREADS) {
        return;
    }
    uint64_t flags = irq_save();
    printk(LOG_TRACE, "[SCHED] Removing thread TID %d\n", thread_id);
    thread_table[thread_id].state = TASK_STATE_DEAD;
    thread_table[thread_id].rsp = 0;
    thread_table[thread_id].user_rsp = 0;
    irq_restore(flags);
}

void init_scheduler(void) {
    printk(LOG_TRACE, "[SCHED] Initializing scheduler table (%d max threads, %d max procs)\n", MAX_THREADS, MAX_PROCESSES);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i].active = false;
    }
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_table[i].state = TASK_STATE_DEAD;
        thread_table[i].rsp = 0;
        thread_table[i].user_rsp = 0;
        thread_table[i].fs_base = 0;
        init_fpu_context(&thread_table[i]);
    }

    struct process *kproc = create_process("kernel", vmm_get_current_pml4(), 0, 0);
    thread_table[0].tid = 0;
    thread_table[0].process = kproc;
    thread_table[0].state = TASK_STATE_RUNNING;
    current_thread_id = 0;

    uint64_t kstack_canonical = (uint64_t)&thread_table[0].kernel_stack[KERNEL_STACK_SIZE];
    global_tss.rsp0 = (uint64_t)(kstack_canonical & ~0xFULL);
}

void start_scheduler(void) {
    printk(LOG_TRACE, "[SCHED] Starting scheduler loop...\n");
    uint64_t safe_kernel_stack = ((uint64_t)&thread_table[0].kernel_stack[KERNEL_STACK_SIZE] & ~0xFULL) - 16;

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

uint64_t terminate(uint64_t current_rsp, int pid) {
    if (pid < 0 || pid >= MAX_PROCESSES) {
        return current_rsp;
    }
    printk(LOG_TRACE, "[SYSCALL] Terminate called on PID %d\n", pid);
    process_table[pid].active = false;

    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].process != NULL && thread_table[i].process->pid == pid) {
            thread_table[i].state = TASK_STATE_ZOMBIE;
        }
    }

    int parent_pid = process_table[pid].parent_pid;
    if (parent_pid >= 0 && parent_pid < MAX_PROCESSES) {
        for (int i = 0; i < MAX_THREADS; i++) {
            if (thread_table[i].process != NULL &&
                thread_table[i].process->pid == parent_pid &&
                thread_table[i].state == TASK_STATE_WAITING) {
                thread_table[i].state = TASK_STATE_READY;
            }
        }
    }
    return schedule_preemptive(current_rsp);
}

uint64_t syscall_exit_handler(uint64_t current_rsp, uint64_t status) {
    (void)status;
    printk(LOG_TRACE, "exit called on PID: %d\n", getpid());
    int result = terminate(current_rsp, getpid());
    int parent_pid = thread_table[current_thread_id].process->parent_pid;
    if (parent_pid >= 0 && parent_pid < MAX_PROCESSES) {
        for (int i = 0; i < MAX_THREADS; i++) {
            if (thread_table[i].process != NULL &&
                thread_table[i].process->pid == parent_pid &&
                thread_table[i].state == TASK_STATE_WAITING) {
                printk(LOG_TRACE, "[SYSCALL] Waking parent PID %d thread TID %d from WAITING state\n", parent_pid, i);
                thread_table[i].state = TASK_STATE_READY;
            }
        }
    }
    return result;
}

int getpid() {
    if (thread_table[current_thread_id].process != NULL) {
        return thread_table[current_thread_id].process->pid;
    }
    return -1;
}

int gettid() {
    return thread_table[current_thread_id].tid;
}

static void ipc_pause(void) {
    asm volatile("int $0x20" ::: "memory");
}

int ipc_send(uint32_t target_pid, const void *buf, uint32_t size) {
    if (target_pid >= MAX_PROCESSES || size > MAX_MSG_PAYLOAD || target_pid == (uint32_t)getpid() || buf == NULL) {
        printk(LOG_ERROR, "[IPC_ERR] Invalid send args: src=%d, target=%d, size=%u, buf=%p\n", getpid(), target_pid, size, buf);
        return -1;
    }
    if (!is_valid_user_pointer(buf, size)) {
        printk(LOG_ERROR, "[IPC_ERR] Invalid user buffer pointer: %p (size %u)\n", buf, size);
        return -1;
    }

    while (1) {
        uint64_t flags = irq_save();
        struct process *target = &process_table[target_pid];

        if (!target->active) {
            irq_restore(flags);
            return -1;
        }

        if (target->msg_count < MAX_PROCESS_MSGS) {
            int tail = target->msg_tail;
            kernel_msg_t *msg = &target->msg_queue[tail];

            msg->sender_pid = getpid();
            msg->payload_size = size;

            safe_memcpy(msg->data, buf, size);

            target->msg_tail = (tail + 1) % MAX_PROCESS_MSGS;
            target->msg_count++;

            for (int i = 0; i < MAX_THREADS; i++) {
                if (thread_table[i].process == target && thread_table[i].state == TASK_STATE_BLOCKED_RECEIVE) {
                    thread_table[i].state = TASK_STATE_READY;
                }
            }

            irq_restore(flags);
            return 0;
        }
        thread_table[current_thread_id].state = TASK_STATE_BLOCKED_SEND;
        irq_restore(flags);

        ipc_pause();
    }
}

int ipc_recv(void *buf, uint32_t max_size, uint32_t *out_sender_pid) {
    if (buf == NULL || !is_valid_user_pointer(buf, max_size)) {
        printk(LOG_ERROR, "[IPC_ERR] Invalid recv buffer pointer: %p (max_size %u)\n", buf, max_size);
        return -1;
    }

    while (1) {
        uint64_t flags = irq_save();
        struct process *current_proc = thread_table[current_thread_id].process;

        if (current_proc->msg_count > 0) {
            int head = current_proc->msg_head;
            kernel_msg_t *msg = &current_proc->msg_queue[head];

            uint32_t bytes_to_copy = msg->payload_size;
            if (bytes_to_copy > max_size) {
                bytes_to_copy = max_size;
            }

            safe_memcpy(buf, msg->data, bytes_to_copy);
            if (out_sender_pid && is_valid_user_pointer(out_sender_pid, sizeof(uint32_t))) {
                *out_sender_pid = msg->sender_pid;
            }

            current_proc->msg_head = (head + 1) % MAX_PROCESS_MSGS;
            current_proc->msg_count--;

            for (int i = 0; i < MAX_THREADS; i++) {
                if (thread_table[i].state == TASK_STATE_BLOCKED_SEND) {
                    thread_table[i].state = TASK_STATE_READY;
                }
            }

            irq_restore(flags);
            return bytes_to_copy;
        }

        thread_table[current_thread_id].state = TASK_STATE_BLOCKED_RECEIVE;
        irq_restore(flags);

        ipc_pause();
    }
}

int ipc_send_nonblock(uint32_t target_pid, const void *buf, uint32_t size) {
    if (target_pid >= MAX_PROCESSES || size > MAX_MSG_PAYLOAD || target_pid == (uint32_t)getpid() || buf == NULL) {
        return -1;
    }
    if (!is_valid_user_pointer(buf, size)) {
        return -1;
    }

    uint64_t flags = irq_save();
    struct process *target = &process_table[target_pid];

    if (!target->active) {
        irq_restore(flags);
        return -1;
    }

    if (target->msg_count >= MAX_PROCESS_MSGS) {
        printk(LOG_WARNING, "[IPC] Non-blocking send from PID %d to PID %d would block\n", getpid(), target_pid);
        irq_restore(flags);
        return ERR_IPC_WOULD_BLOCK;
    }

    int tail = target->msg_tail;
    kernel_msg_t *msg = &target->msg_queue[tail];

    msg->sender_pid = getpid();
    msg->payload_size = size;
    safe_memcpy(msg->data, buf, size);

    target->msg_tail = (tail + 1) % MAX_PROCESS_MSGS;
    target->msg_count++;

    printk(LOG_TRACE, "[IPC] Non-block delivered msg from PID %d to PID %d\n", getpid(), target_pid);

    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].process == target && thread_table[i].state == TASK_STATE_BLOCKED_RECEIVE) {
            thread_table[i].state = TASK_STATE_READY;
        }
    }

    irq_restore(flags);
    return 0;
}

int ipc_recv_nonblock(void *buf, uint32_t max_size, uint32_t *out_sender_pid) {
    if (buf == NULL || !is_valid_user_pointer(buf, max_size)) {
        return -1;
    }

    uint64_t flags = irq_save();
    struct process *current_proc = thread_table[current_thread_id].process;

    if (current_proc->msg_count == 0) {
        irq_restore(flags);
        return ERR_IPC_WOULD_BLOCK;
    }

    int head = current_proc->msg_head;
    kernel_msg_t *msg = &current_proc->msg_queue[head];

    uint32_t bytes_to_copy = msg->payload_size;
    if (bytes_to_copy > max_size) {
        bytes_to_copy = max_size;
    }

    safe_memcpy(buf, msg->data, bytes_to_copy);
    if (out_sender_pid && is_valid_user_pointer(out_sender_pid, sizeof(uint32_t))) {
        *out_sender_pid = msg->sender_pid;
    }

    current_proc->msg_head = (head + 1) % MAX_PROCESS_MSGS;
    current_proc->msg_count--;

    printk(LOG_TRACE, "[IPC] Non-block consumed msg for PID %d\n", getpid());

    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state == TASK_STATE_BLOCKED_SEND) {
            thread_table[i].state = TASK_STATE_READY;
        }
    }

    irq_restore(flags);
    return bytes_to_copy;
}

int getuid() {
    return thread_table[current_thread_id].process->uid;
}

int getgid() {
    return thread_table[current_thread_id].process->gid;
}

int waitpid(uint64_t pid) {
    if (pid >= MAX_PROCESSES || !process_table[pid].active) {
        return -1;
    }

    printk(LOG_TRACE, "[SCHED] PID %d waitpid waiting on PID %lu\n", getpid(), pid);

    while (process_table[pid].active) {
        uint64_t flags = irq_save();
        thread_table[current_thread_id].state = TASK_STATE_WAITING;
        irq_restore(flags);

        ipc_pause();
    }

    thread_table[current_thread_id].state = TASK_STATE_READY;
    printk(LOG_TRACE, "[SCHED] PID %d finished waitpid on PID %lu\n", getpid(), pid);
    return 0;
}

char* getpcwd() {
    return thread_table[current_thread_id].process->cwd;
}

int ps(struct utask* u, int max_len) {
    int len_found = 0;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!process_table[i].active) {
            continue;
        }

        if (len_found >= max_len) {
            return -1;
        }

        strcpy(u[len_found].cwd, process_table[i].cwd);
        strcpy(u[len_found].name, process_table[i].name);

        u[len_found].gid = process_table[i].gid;
        u[len_found].uid = process_table[i].uid;
        u[len_found].pid = process_table[i].pid;
        u[len_found].parent_pid = process_table[i].parent_pid;
        u[len_found].state = TASK_STATE_RUNNING;

        len_found++;
    }

    return len_found;
}
struct process *get_current_proc(void) {
    uint64_t flags = irq_save();
    struct process *proc = thread_table[current_thread_id].process;
    irq_restore(flags);
    return proc;
}