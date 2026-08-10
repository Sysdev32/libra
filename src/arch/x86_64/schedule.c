// SPDX-License-Identifier: GPL-3.0-only
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <signal.h>
#include <drivers/fb.h>
#include <arch/x86_64/schedule.h>

#include <arch/x86_64/idt.h>

#include "hals/ahci.h"

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

void safe_memcpy(void *dest, const void *src, size_t n) {
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

void init_fpu_context(struct thread *thread) {
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
uint64_t irq_save(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

void irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) {
        asm volatile("sti" ::: "memory");
    }
}

static void ipc_pause(void) {
    asm volatile("int $0x20" ::: "memory");
}

/* Thread Exit and Join Functions */
void sys_thread_exit(int retval) {
    uint64_t flags = irq_save();
    int tid = current_thread_id;
    struct thread *curr = &thread_table[tid];

    curr->exit_code = retval;
    curr->state = TASK_STATE_ZOMBIE;

    // Wake up any threads waiting to join this specific thread
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state == TASK_STATE_WAITING && thread_table[i].joining_tid == tid) {
            thread_table[i].joining_tid = -1;
            thread_table[i].state = TASK_STATE_READY;
        }
    }

    irq_restore(flags);

    // Yield control to force context switch
    ipc_pause();
    for (;;);
}

int sys_thread_join(int tid, int *retval) {
    if (tid < 0 || tid >= MAX_THREADS || tid == current_thread_id) {
        return -1;
    }

    while (1) {
        uint64_t flags = irq_save();
        struct thread *target = &thread_table[tid];

        // Check if the target thread belongs to the current process or is invalid
        if (target->state == TASK_STATE_DEAD) {
            irq_restore(flags);
            return -1;
        }

        if (target->state == TASK_STATE_ZOMBIE) {
            if (retval && is_valid_user_pointer(retval, sizeof(int))) {
                *retval = target->exit_code;
            } else if (retval && target->user_rsp == 0) {
                // Direct kernel pointer assignment if calling from kernel context
                *retval = target->exit_code;
            }

            // Reap zombie thread
            target->state = TASK_STATE_DEAD;
            target->rsp = 0;
            target->user_rsp = 0;
            target->joining_tid = -1;

            irq_restore(flags);
            return 0;
        }

        // Target thread is still running/ready; block current thread
        thread_table[current_thread_id].joining_tid = tid;
        thread_table[current_thread_id].state = TASK_STATE_WAITING;
        irq_restore(flags);

        ipc_pause();
    }
}

void kernel_thread_exit_handler(void) {
    sys_thread_exit(0);
}

void userspace_exit_handler(void) {
    sys_thread_exit(0);
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
    if (!proc || !entry_point) {
        printk(LOG_ERROR, "[SCHED_ERR] Invalid arguments passed to create_thread\n");
        return -1;
    }

    uint64_t flags = irq_save();

    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state == TASK_STATE_DEAD) {
            init_fpu_context(&thread_table[i]);

            thread_table[i].tid = i;
            thread_table[i].process = proc;
            thread_table[i].gs_base = 0;
            thread_table[i].exit_code = 0;
            thread_table[i].joining_tid = -1;
            memset(thread_table[i].tls_slots, 0, sizeof(thread_table[i].tls_slots));

            uint64_t aligned_stack = 0;
            size_t tcb_size_aligned = (sizeof(struct tcb) + 15) & ~0xFULL;

            if (is_user) {
                aligned_stack = ((uint64_t)user_stack) & ~0xFULL;

                if (fs_base != 0) {
                    // Explicit FS_BASE provided (e.g. from custom pthread_create)
                    thread_table[i].fs_base = fs_base & ~0xFULL;
                    thread_table[i].user_rsp = aligned_stack;
                } else if (aligned_stack != 0) {
                    // Place TCB at the top of the allocated stack block
                    thread_table[i].fs_base = (aligned_stack - tcb_size_aligned) & ~0xFULL;
                    // User RSP starts BELOW the TCB so stack pushes don't corrupt TCB data
                    thread_table[i].user_rsp = thread_table[i].fs_base;
                } else {
                    thread_table[i].fs_base = 0;
                    thread_table[i].user_rsp = 0;
                }
            } else {
                thread_table[i].user_rsp = 0;

                if (fs_base == 0) {
                    thread_table[i].tcb.self = &thread_table[i].tcb;
                    thread_table[i].fs_base = (uint64_t)&thread_table[i].tcb;
                } else {
                    thread_table[i].fs_base = fs_base;
                }
            }

            // Setup Kernel Stack (Must match kernel assembly ISR frame sizing)
            uintptr_t k_stack_raw = (uintptr_t)&thread_table[i].kernel_stack[KERNEL_STACK_SIZE];
            uint64_t *kernel_stack_top = (uint64_t *)(k_stack_raw & ~0xFULL);

            if (!is_user) {
                kernel_stack_top[-1] = (uint64_t)kernel_thread_exit_handler;
            }

            // Exactly 24 qwords allocated for trap frame matching kernel ISR assembly layout
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

    // Align signal frame stack insertion to 16 bytes before forcing user stack pushing
    uint64_t new_user_rsp = (old_rsp - sizeof(uint64_t)) & ~0xFULL;
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
    fpu_context_save();
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
            printk(LOG_TRACE, "[SCHED] Reaping unjoined dead thread TID %d\n", old_thread_id);
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

    uint64_t kstack_canonical = (uint64_t)&curr_thread->kernel_stack[KERNEL_STACK_SIZE];
    global_tss.rsp0 = (uint64_t)(kstack_canonical & ~0xFULL);
    fpu_context_restore();
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
    thread_table[thread_id].joining_tid = -1;
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
        thread_table[i].exit_code = 0;
        thread_table[i].joining_tid = -1;
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
#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE          0x1000ULL
#define PAGE_PRESENT       (1ULL << 0)
#define PAGE_HUGE          (1ULL << 7)
#define PAGE_FRAME_MASK    0x000FFFFFFFFFF000ULL
#define PAGE_FLAGS_MASK    0xFFF0000000000FFFULL

#ifndef HHDM_OFFSET
extern uint64_t g_hhdm_offset;
#define HHDM_OFFSET g_hhdm_offset
#endif

#define PHYS_TO_VIRT(phys) ((void *)((uint64_t)(phys) + HHDM_OFFSET))
#define VIRT_TO_PHYS(virt) ((uint64_t)(virt) - HHDM_OFFSET)
// Static physical frame pool inside the file
#define MAX_POOL_PAGES 4096
static _Alignas(PAGE_SIZE) uint8_t g_static_frame_pool[MAX_POOL_PAGES][PAGE_SIZE];
static size_t g_pool_index = 0;

static inline uint64_t alloc_static_frame(void) {
    if (g_pool_index >= MAX_POOL_PAGES) {
        // Pool exhausted
        return 0;
    }
    uint8_t *page_virt = g_static_frame_pool[g_pool_index++];

    // Zero out the frame
    uint64_t *ptr = (uint64_t *)page_virt;
    for (size_t i = 0; i < 512; i++) {
        ptr[i] = 0;
    }

    return VIRT_TO_PHYS(page_virt);
}

static inline void copy_phys_frame(uint64_t dst_phys, uint64_t src_phys) {
    uint64_t *dst = (uint64_t *)PHYS_TO_VIRT(dst_phys);
    uint64_t *src = (uint64_t *)PHYS_TO_VIRT(src_phys);
    for (size_t i = 0; i < 512; i++) {
        dst[i] = src[i];
    }
}

static void clone_table_level(uint64_t src_table_phys, uint64_t dst_table_phys, int level) {
    uint64_t *src_table = (uint64_t *)PHYS_TO_VIRT(src_table_phys);
    uint64_t *dst_table = (uint64_t *)PHYS_TO_VIRT(dst_table_phys);

    int max_entries = (level == 4) ? 256 : 512;

    for (int i = 0; i < max_entries; i++) {
        uint64_t entry = src_table[i];
        if (!(entry & PAGE_PRESENT)) {
            continue;
        }

        uint64_t src_frame_phys = entry & PAGE_FRAME_MASK;
        uint64_t flags          = entry & PAGE_FLAGS_MASK;

        // Leaf entry: 4KiB page at Level 1, or Huge Page at Level 2/3
        if (level == 1 || (entry & PAGE_HUGE)) {
            uint64_t child_frame_phys = alloc_static_frame();
            copy_phys_frame(child_frame_phys, src_frame_phys);
            dst_table[i] = child_frame_phys | flags;
        }
        // Intermediate level table
        else {
            uint64_t child_table_phys = alloc_static_frame();
            clone_table_level(src_frame_phys, child_table_phys, level - 1);
            dst_table[i] = child_table_phys | flags;
        }
    }
}

void vmm_clone_pml4(uint64_t old_pml4_phys, uint64_t new_pml4_phys) {
    // 1. Deep copy user space (PML4 entries 0 - 255)
    clone_table_level(old_pml4_phys, new_pml4_phys, 4);

    // 2. Share kernel space (PML4 entries 256 - 511)
    uint64_t *src_pml4 = (uint64_t *)PHYS_TO_VIRT(old_pml4_phys);
    uint64_t *dst_pml4 = (uint64_t *)PHYS_TO_VIRT(new_pml4_phys);

    for (int i = 256; i < 512; i++) {
        dst_pml4[i] = src_pml4[i];
    }
}
struct process *get_current_proc(void) {
    uint64_t flags = irq_save();
    struct process *proc = thread_table[current_thread_id].process;
    irq_restore(flags);
    return proc;
}
static inline uint64_t read_cr3(void)
{
    uint64_t cr3;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}
void vmm_map_range(page_table_t *pml4, uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
    if (size == 0) return;

    // Align base addresses down to page boundaries
    uint64_t virt_start = virt & ~(PAGE_SIZE - 1);
    uint64_t phys_start = phys & ~(PAGE_SIZE - 1);

    // Calculate total size accounting for unaligned starting offset
    uint64_t offset = virt & (PAGE_SIZE - 1);
    uint64_t aligned_size = (size + offset + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t i = 0; i < aligned_size; i += PAGE_SIZE) {
        vmm_map_page(pml4, virt_start + i, phys_start + i, flags);
    }
}
#define PTE_PRESENT (1ULL << 0)
#define PTE_USER     (1ULL << 2)
int fork(uint64_t current_rsp) {
    uint64_t flags = irq_save();

    struct thread *parent_thread = &thread_table[current_thread_id];
    struct process *parent_proc = parent_thread->process;

    if (!parent_proc || !parent_thread) {
        irq_restore(flags);
        return -1;
    }

    // 1. Allocate process slot for child
    struct process *child_proc = NULL;
    int child_pid = -1;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!process_table[i].active) {
            child_pid = i;
            child_proc = &process_table[i];
            break;
        }
    }

    if (!child_proc) {
        printk(LOG_ERROR, "[FORK_ERR] No free process slots\n");
        irq_restore(flags);
        return -1;
    }

    // 2. Allocate thread slot for child
    int child_tid = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state == TASK_STATE_DEAD) {
            child_tid = i;
            break;
        }
    }

    if (child_tid == -1) {
        printk(LOG_ERROR, "[FORK_ERR] No free thread slots for single child thread\n");
        irq_restore(flags);
        return -1;
    }

    // 3. Create new address space and clone current mappings
    page_table_t *new_pml4 = vmm_create_address_space();
    if (!new_pml4) {
        printk(LOG_ERROR, "[FORK_ERR] Failed to allocate child PML4\n");
        irq_restore(flags);
        return -1;
    }

    vmm_clone_pml4(read_cr3(), (uint64_t)VIRT_TO_PHYS(new_pml4));

    // 4. Handle isolated physical stack allocation & copy
    uint64_t stack_size = 64 * PAGE_SIZE;
    uint64_t stack_virt_base = 0x600000;
    uint64_t new_stack_phys = stack_alloc(stack_size);

    if (!new_stack_phys) {
        printk(LOG_ERROR, "[FORK_ERR] Failed to allocate child stack memory\n");
        irq_restore(flags);
        return -1;
    }

    // Copy live parent user stack content into the child's new physical backing
    uint8_t *src_stack = (uint8_t *)stack_virt_base;
    uint8_t *dst_stack = (uint8_t *)PHYS_TO_VIRT(new_stack_phys);
    safe_memcpy(dst_stack, src_stack, stack_size);

    // Override the stack region in child PML4 with the new physical frames
    vmm_map_range(
        new_pml4,
        stack_virt_base,
        new_stack_phys,
        stack_size,
        PTE_WRITABLE | PTE_USER | PTE_PRESENT
    );

    // 5. Populate child process metadata
    child_proc->pid = child_pid;
    child_proc->pml4 = new_pml4;
    child_proc->uid = parent_proc->uid;
    child_proc->gid = parent_proc->gid;
    child_proc->parent_pid = parent_proc->pid;
    child_proc->msg_head = 0;
    child_proc->msg_tail = 0;
    child_proc->msg_count = 0;
    child_proc->pending_signals = 0;
    child_proc->sig_mask = parent_proc->sig_mask;

    safe_memcpy(child_proc->signal_handlers, parent_proc->signal_handlers, sizeof(child_proc->signal_handlers));
    safe_memcpy(child_proc->cwd, parent_proc->cwd, sizeof(child_proc->cwd));
    strcpy(child_proc->name, parent_proc->name);
    child_proc->active = true;

    // 6. Populate child thread context
    struct thread *child_thread = &thread_table[child_tid];
    child_thread->tid = child_tid;
    child_thread->process = child_proc;
    child_thread->user_rsp = parent_thread->user_rsp;
    child_thread->fs_base = parent_thread->fs_base;
    child_thread->gs_base = parent_thread->gs_base;
    child_thread->exit_code = 0;
    child_thread->joining_tid = -1;

    safe_memcpy(child_thread->tls_slots, parent_thread->tls_slots, sizeof(parent_thread->tls_slots));
    safe_memcpy(child_thread->fpu_state, parent_thread->fpu_state, sizeof(child_thread->fpu_state));

    // 7. Construct kernel trap frame for child thread execution
    uintptr_t child_kstack_top = (uintptr_t)&child_thread->kernel_stack[KERNEL_STACK_SIZE];
    uint64_t *child_kstack_aligned = (uint64_t *)(child_kstack_top & ~0xFULL);

    uint64_t *child_ctx = child_kstack_aligned - 24;
    uint64_t *parent_ctx = (uint64_t *)current_rsp;

    // Copy parent's saved register context to child kernel stack
    safe_memcpy(child_ctx, parent_ctx, 24 * sizeof(uint64_t));

    // Set child return value (RAX slot) to 0
    child_ctx[0] = 0;

    child_thread->rsp = (uint64_t)child_ctx;
    child_thread->state = TASK_STATE_READY;

    printk(LOG_TRACE, "[FORK] PID %d (TID %d) forked single-threaded child PID %d (TID %d)\n",
           parent_proc->pid, parent_thread->tid, child_pid, child_tid);

    irq_restore(flags);

    // Parent return: returns child's PID
    return child_pid;
}