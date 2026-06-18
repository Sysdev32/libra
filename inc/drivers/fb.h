// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <vendor/flanterm/flanterm.h>
#include <vendor/flanterm/flanterm_backends/fb.h>
#include <stddef.h>
#include <stdarg.h>
typedef enum {
    LOG_WARNING,
    LOG_ERROR,
    LOG_TRACE,
    LOG_DEBUG,
    LOG_INFO,
    LOG_ACPI,
    LOG_NONE
} LogType;
void initConsole(struct flanterm_context *ft_ctx);
void printk(LogType type, const char* fmt, ...);