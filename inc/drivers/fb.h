// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <vendor/flanterm/flanterm.h>
#include <vendor/flanterm/flanterm_backends/fb.h>
#include <stddef.h>
#include <stdarg.h>
void initConsole(struct flanterm_context *ft_ctx);
void printk(const char* fmt, ...);