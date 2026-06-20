// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <vendor/flanterm/flanterm.h>
#include <vendor/flanterm/flanterm_backends/fb.h>
#include <stddef.h>
#include <stdarg.h>
#include <limine.h>
typedef enum {
    LOG_WARNING,
    LOG_ERROR,
    LOG_TRACE,
    LOG_DEBUG,
    LOG_INFO,
    LOG_ACPI,
    LOG_NONE
} LogType;
void initConsole(struct flanterm_context *ft_ctx, struct limine_framebuffer* fb);
void printk(LogType type, const char* fmt, ...);
void draw_rect(int rect_x, int rect_y, int rect_width, int rect_height, 
                   uint8_t r, uint8_t g, uint8_t b);
void graduate();