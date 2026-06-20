// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
int create_kernel_task(void (*entry_point)(void));
int create_user_task(void (*entry_point)(void), void* allocated_user_stack);
uint64_t schedule_preemptive(uint64_t old_rsp);
void init_scheduler(void);
void start_scheduler(void);
void fpu_context_save(void);
void fpu_context_restore(void);
