// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

#include "drivers/alloc.h"
#define MAX_PROCESSES          128
#define MAX_THREADS            512
#define KERNEL_STACK_SIZE      4096
#define HHDM_OFFSET            0xffff800000000000ULL
#define MAX_MSG_PAYLOAD        1024
#define MAX_PROCESS_MSGS       16
#define ERR_IPC_WOULD_BLOCK   -2

/* Model Specific Registers (MSR) for TLS */
#define IA32_FS_BASE           0xC0000100ULL
#define IA32_GS_BASE           0xC0000101ULL
#define IA32_KERNEL_GS_BASE    0xC0000102ULL
#define MAX_TLS_KEYS 16
typedef enum {
    TASK_STATE_DEAD = 0,
    TASK_STATE_READY,
    TASK_STATE_RUNNING,
    TASK_STATE_ZOMBIE,
    TASK_STATE_BLOCKED_SEND,    // Added: Waiting for space in a target queue
    TASK_STATE_BLOCKED_RECEIVE,
    TASK_STATE_WAITING
} task_state_t;
struct utask {
    task_state_t state;
    int uid;
    int gid;
    int parent_pid;
    int pid;
    char name[32];
    char cwd[32];
};

typedef struct {
    uint32_t sender_pid;
    uint32_t payload_size;
    uint8_t  data[MAX_MSG_PAYLOAD];
} kernel_msg_t;

struct process {
    int pid;
    page_table_t *pml4;
    int uid;
    int gid;
    int parent_pid;
    char name[32];
    char cwd[512];
    kernel_msg_t msg_queue[MAX_PROCESS_MSGS];
    int msg_head;
    int msg_tail;
    int msg_count;
    uint64_t signal_handlers[32];
    uint32_t pending_signals;
    uint32_t sig_mask;
    bool active;
};
typedef struct {
    void *self; // x86_64 ABI requirement: offset 0x0 points to itself
} kernel_tcb_t;
struct thread {
    int tid;
    struct process *process;
    uint64_t rsp;
    uint64_t user_rsp;
    uint64_t fs_base;
    uint64_t gs_base;
    task_state_t state;
    uint8_t fpu_state[512] __attribute__((aligned(16)));
    uint8_t kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));
    kernel_tcb_t tcb;
    uint64_t tls_slots[MAX_TLS_KEYS];
    int exit_code;         // Exit return value for thread_join
    int joining_tid;
};
struct tcb {
    struct tcb *self;        /* FS:0x00 - Pointer to self (required by x86_64 TLS ABI) */
    uint64_t dtv;            /* FS:0x08 - Dynamic Thread Vector pointer (for dynamic loading/modules) */
    uint64_t thread_id;      /* FS:0x10 - Thread ID (TID) */
    int32_t  multiple_threads;/* FS:0x18 - Multi-threading flag */
    int32_t  gscope_flag;    /* FS:0x1C - Global scope flag */
    uint64_t sysinfo;        /* FS:0x20 - System call entry / kernel info pointer */
    uint64_t stack_guard;    /* FS:0x28 - Stack canary for GCC -fstack-protector */
    uint64_t pointer_guard;  /* FS:0x30 - Pointer mangling salt */

    /* Custom OS / Userland thread-local data slots */
    void    *tls_data;       /* FS:0x38 - Custom TLS user payload pointer */
    int      errno_val;      /* FS:0x40 - Per-thread errno */
    uint32_t reserved;       /* Padding / Alignment to 64 bytes */
} __attribute__((aligned(16)));
int create_kernel_task(void (*entry_point)(void), char* name);
int create_user_task(void (*entry_point)(void), void* user_stack, uint64_t rdi, uint64_t rsi, void *pml4, int uid, int gid, int pid, char* name);
uint64_t schedule_preemptive(uint64_t old_rsp);
void init_scheduler(void);
void start_scheduler(void);
void fpu_context_save(void);
void fpu_context_restore(void);
uint64_t syscall_exit_handler(uint64_t current_rsp, uint64_t status);
int ipc_send(uint32_t target_pid, const void *buf, uint32_t size);
int ipc_recv(void *buf, uint32_t max_size, uint32_t *out_sender_pid);
uint64_t terminate(uint64_t current_rsp, int pid);
int getuid();
int getgid();
int waitpid(uint64_t pid);
int spawn(const char *path, int argc, char **argv, char* name);
int ipc_recv_nonblock(void *buf, uint32_t max_size, uint32_t *out_sender_pid);
int ipc_send_nonblock(uint32_t target_pid, const void *buf, uint32_t size);
int ps(struct utask* u, const int max_len);
char* getpcwd();
void set_cwd(char* cwdi);
unsigned long long get_launchd_pid(void);
uint64_t set_signal_handler(int sig, uint64_t handler);
int send_signal(int pid, int sig);
int getpid();
struct process *get_current_proc(void);
struct process *create_process(const char *name, page_table_t *pml4, int uid, int gid);
int create_thread(struct process *proc, void (*entry_point)(void), void *user_stack, uint64_t rdi, uint64_t rsi, uint64_t fs_base, bool is_user);
int clone(void (*fn)(void *), void *user_stack, void *arg, bool is_user);