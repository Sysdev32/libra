// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
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
int spawn(const char *path, int argc, char **argv, char* name);
int ipc_recv_nonblock(void *buf, uint32_t max_size, uint32_t *out_sender_pid);
int ipc_send_nonblock(uint32_t target_pid, const void *buf, uint32_t size);
int ps(struct utask* u, int max_len);
