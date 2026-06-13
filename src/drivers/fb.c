// SPDX-License-Identifier: GPL-3.0-only
#include <drivers/fb.h>
#include <string.h>
struct flanterm_context *ctx;
void initConsole(struct flanterm_context *ft_ctx) {
    ctx = ft_ctx;
}
static char *itoa(uint64_t value, char *str, int base, int uppercase) {
    char *rc = str;
    char *ptr = str;
    char *low;
    
    // Check for supported bases
    if (base < 2 || base > 36) {
        *str = '\0';
        return str;
    }

    // Set up the character mapping array
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    // Process digits in reverse order
    do {
        *ptr++ = digits[value % base];
        value /= base;
    } while (value);

    *ptr = '\0';

    // Reverse the string in-place
    low = rc;
    ptr--;
    while (low < ptr) {
        char tmp = *low;
        *low++ = *ptr;
        *ptr-- = tmp;
    }

    return rc;
}

int vsprintf(char *buf, const char *fmt, va_list args) {
    char *p = buf;
    const char *f = fmt;

    while (*f) {
        if (*f != '%') {
            *p++ = f[0];
            f++;
            continue;
        }

        f++; // Skip '%'

        // Handle direct escapes (%%)
        if (*f == '%') {
            *p++ = '%';
            f++;
            continue;
        }

        switch (*f) {
            case 'c': {
                char c = (char)va_arg(args, int);
                *p++ = c;
                break;
            }

            case 's': {
                char *s = va_arg(args, char *);
                if (!s) s = "(null)";
                while (*s) {
                    *p++ = *s++;
                }
                break;
            }

            case 'd':
            case 'i': {
                int64_t d = va_arg(args, int);
                char tmp[32];
                if (d < 0) {
                    *p++ = '-';
                    d = -d;
                }
                itoa(d, tmp, 10, 0);
                char *t = tmp;
                while (*t) *p++ = *t++;
                break;
            }

            case 'u': {
                uint64_t u = va_arg(args, unsigned int);
                char tmp[32];
                itoa(u, tmp, 10, 0);
                char *t = tmp;
                while (*t) *p++ = *t++;
                break;
            }

            case 'x':
            case 'X': {
                uint64_t x = va_arg(args, unsigned int);
                char tmp[32];
                itoa(x, tmp, 16, (*f == 'X'));
                char *t = tmp;
                while (*t) *p++ = *t++;
                break;
            }

            case 'p': {
                uint64_t ptr_val = (uint64_t)va_arg(args, void *);
                char tmp[32];
                *p++ = '0';
                *p++ = 'x';
                itoa(ptr_val, tmp, 16, 0);
                char *t = tmp;
                while (*t) *p++ = *t++;
                break;
            }

            default:
                // Unknown specifier; print raw characters to avoid breaking layouts
                *p++ = '%';
                *p++ = *f;
                break;
        }
        f++;
    }

    *p = '\0';
    return (int)(p - buf); // Return string length
}
void printk(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    
    va_start(args, fmt);
    int len = vsprintf(buf, fmt, args);
    va_end(args);

    // Loop through the formatted string buffer
    for (int i = 0; i < len; i++) {
        // If we hit a raw '\n', send a '\r' first to reset the cursor to the left
        if (buf[i] == '\n') {
            flanterm_write(ctx, "\r", 1);
            
            // Wait for COM1 serial port to be ready for '\r'
            uint8_t status;
            do {
                __asm__ volatile("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)0x3FD));
            } while ((status & 0x20) == 0);
            
            // Transmit '\r' to COM1 data port (0x3F8)
            __asm__ volatile("outb %0, %1" :: "a"((uint8_t)'\r'), "Nd"((uint16_t)0x3F8));
        }
        
        // Write the original character to the terminal screen (including the '\n')
        flanterm_write(ctx, &buf[i], 1);

        // Wait for COM1 serial port to be ready for the current character
        uint8_t status;
        do {
            __asm__ volatile("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)0x3FD));
        } while ((status & 0x20) == 0);
        
        // Transmit the current character byte to COM1 data port (0x3F8)
        __asm__ volatile("outb %0, %1" :: "a"((uint8_t)buf[i]), "Nd"((uint16_t)0x3F8));
    }
}