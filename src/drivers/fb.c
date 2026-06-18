// SPDX-License-Identifier: GPL-3.0-only
#include <drivers/fb.h>
#include <string.h>
#include <stdarg.h>
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
#include <stdarg.h>
#include <stdint.h>

static const char* parse_length(const char *f, int *is_long, int *is_longlong)
{
    *is_long = 0;
    *is_longlong = 0;

    if (*f == 'l') {
        f++;
        if (*f == 'l') {
            *is_longlong = 1;
            return f + 1;
        }
        *is_long = 1;
        return f;
    }

    return f;
}

static const char* parse_format(const char *f, int *zero, int *width)
{
    *zero = 0;
    *width = 0;

    if (*f == '0') {
        *zero = 1;
        f++;
    }

    while (*f >= '0' && *f <= '9') {
        *width = (*width * 10) + (*f - '0');
        f++;
    }

    return f;
}

static int str_len(const char *s)
{
    int i = 0;
    while (s[i]) i++;
    return i;
}

static void pad(char **buf, const char *s, int width, int zero)
{
    int len = str_len(s);

    while (len < width) {
        *(*buf)++ = zero ? '0' : ' ';
        width--;
    }

    while (*s) {
        *(*buf)++ = *s++;
    }
}

int vsprintf(char *buf, const char *fmt, va_list args)
{
    char *p = buf;

    while (*fmt) {
        if (*fmt != '%') {
            *p++ = *fmt++;
            continue;
        }

        fmt++; // skip %

        if (*fmt == '%') {
            *p++ = '%';
            fmt++;
            continue;
        }

        int is_long = 0;
        int is_longlong = 0;
        int zero = 0;
        int width = 0;

        fmt = parse_length(fmt, &is_long, &is_longlong);
        fmt = parse_format(fmt, &zero, &width);

        switch (*fmt) {

            case 'c': {
                *p++ = (char)va_arg(args, int);
                break;
            }

            case 's': {
                char *s = va_arg(args, char*);
                if (!s) s = "(null)";
                pad(&p, s, width, 0);
                break;
            }

            case 'd':
            case 'i': {
                long long v;

                if (is_longlong)
                    v = va_arg(args, long long);
                else if (is_long)
                    v = va_arg(args, long);
                else
                    v = va_arg(args, int);

                char tmp[32];
                int neg = (v < 0);

                if (neg) v = -v;

                itoa(v, tmp, 10, 0);

                if (neg) {
                    if (zero && width > 0) {
                        *p++ = '-';
                        pad(&p, tmp, width - 1, 1);
                    } else {
                        char full[64];
                        full[0] = '-';

                        int i = 0;
                        while (tmp[i]) {
                            full[i + 1] = tmp[i];
                            i++;
                        }
                        full[i + 1] = 0;

                        pad(&p, full, width, zero);
                    }
                } else {
                    pad(&p, tmp, width, zero);
                }

                break;
            }

            case 'u': {
                unsigned long long v;

                if (is_longlong)
                    v = va_arg(args, unsigned long long);
                else if (is_long)
                    v = va_arg(args, unsigned long);
                else
                    v = va_arg(args, unsigned int);

                char tmp[32];
                itoa(v, tmp, 10, 0);
                pad(&p, tmp, width, zero);
                break;
            }

            case 'x':
            case 'X': {
                unsigned long long v;

                if (is_longlong)
                    v = va_arg(args, unsigned long long);
                else if (is_long)
                    v = va_arg(args, unsigned long);
                else
                    v = va_arg(args, unsigned int);

                char tmp[32];
                itoa(v, tmp, 16, (*fmt == 'X'));
                pad(&p, tmp, width, zero);
                break;
            }

            case 'p': {
                unsigned long long v = (unsigned long long)va_arg(args, void*);

                char tmp[32];
                tmp[0] = '0';
                tmp[1] = 'x';

                itoa(v, tmp + 2, 16, 0);
                pad(&p, tmp, width ? width : 2, zero);
                break;
            }

            default:
                *p++ = '%';
                *p++ = *fmt;
                break;
        }

        fmt++;
    }

    *p = '\0';
    return (int)(p - buf);
}
void printk(LogType type, const char *fmt, ...) {
    char buf[1024];
    va_list args;
    
    va_start(args, fmt);
    int len = vsprintf(buf, fmt, args);
    va_end(args);
    
    // If no flanterm context is available, send directly to serial (COM1).
    if (ctx == NULL) {
        for (int i = 0; i < len; i++) {
            if (buf[i] == '\n') {
                // Send CR before LF
                uint8_t status;
                do {
                    __asm__ volatile("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)0x3FD));
                } while ((status & 0x20) == 0);
                __asm__ volatile("outb %0, %1" :: "a"((uint8_t)'\r'), "Nd"((uint16_t)0x3F8));
            }

            uint8_t status;
            do {
                __asm__ volatile("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)0x3FD));
            } while ((status & 0x20) == 0);
            __asm__ volatile("outb %0, %1" :: "a"((uint8_t)buf[i]), "Nd"((uint16_t)0x3F8));
        }
        return;
    }
    int color = 0;
    bool bright = true;
    char *text = kcalloc(sizeof(char), 11);
    text = " info   ";
    if (type == LOG_DEBUG) {
        color = 4;
        bright = false; 
        text = " debug  ";
    } else if (type == LOG_ERROR) {
        color = 1;
        bright = false;
        text = " error  ";
    } else if (type == LOG_WARNING) {
        color = 3;
        bright = false;
        text = " warning  ";
    } else if (type == LOG_ACPI) {
        color = 3;
        bright = false;
        text = " uACPI  ";
    } else if (type == LOG_NONE) {
        color = 0;
        bright = false;
        text = "";
    }
    flanterm_set_text_bg(ctx, color, bright);
    flanterm_write(ctx, text, strlen(text));
    flanterm_set_text_bg(ctx, 0, false);
    flanterm_write(ctx, " ", 1);
    // Otherwise write through flanterm and also mirror to serial for physical COM1
    for (int i = 0; i < len; i++) {
        
        if (buf[i] == '\n') {
            flanterm_write(ctx, "\r", 1);
            uint8_t status;
            do {
                __asm__ volatile("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)0x3FD));
            } while ((status & 0x20) == 0);
            __asm__ volatile("outb %0, %1" :: "a"((uint8_t)'\r'), "Nd"((uint16_t)0x3F8));
        }
        
        flanterm_write(ctx, &buf[i], 1);

        uint8_t status;
        do {
            __asm__ volatile("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)0x3FD));
        } while ((status & 0x20) == 0);
        __asm__ volatile("outb %0, %1" :: "a"((uint8_t)buf[i]), "Nd"((uint16_t)0x3F8));
    }
}