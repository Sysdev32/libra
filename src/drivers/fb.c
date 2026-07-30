// SPDX-License-Identifier: GPL-3.0-only
#include <drivers/fb.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

struct flanterm_context *ctx;
struct limine_framebuffer* fb;
bool grad = false;

static uint64_t printk_irq_save(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void printk_irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) {
        asm volatile("sti" ::: "memory");
    }
}

void serial_write_char(char ch) {
    uint8_t status;
    do {
        asm volatile("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)0x3FD));
    } while ((status & 0x20) == 0);
    asm volatile("outb %0, %1" :: "a"((uint8_t)ch), "Nd"((uint16_t)0x3F8));
}

void initConsole(struct flanterm_context *ft_ctx, struct limine_framebuffer* frb) {
    ctx = ft_ctx;
    fb = frb;
}

char *itoa(uint64_t value, char *str, int base, int uppercase) {
    char *rc = str;
    char *ptr = str;
    char *low;
    
    if (base < 2 || base > 36) {
        *str = '\0';
        return str;
    }

    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    do {
        *ptr++ = digits[value % base];
        value /= base;
    } while (value);

    *ptr = '\0';

    low = rc;
    ptr--;
    while (low < ptr) {
        char tmp = *low;
        *low++ = *ptr;
        *ptr-- = tmp;
    }

    return rc;
}

static const char* parse_length(const char *f, int *is_long, int *is_longlong) {
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

static const char* parse_format(const char *f, int *zero, int *width) {
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

static int str_len(const char *s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

void get_pixel(int x, int y, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (!fb || !fb->address) return;
    if (x < 0 || y < 0 || x >= fb->width || y >= fb->height) return;

    int bytes_per_pixel = fb->bpp / 8;
    uint8_t *pixel = (uint8_t *)fb->address + (y * fb->pitch) + (x * bytes_per_pixel);

    // Assumes RGB/BGR order depending on system architecture
    *r = pixel[0];
    *g = pixel[1];
    *b = pixel[2];
}

static void pad(char **buf, const char *s, int width, int zero) {
    int len = str_len(s);

    while (len < width) {
        *(*buf)++ = zero ? '0' : ' ';
        width--;
    }

    while (*s) {
        *(*buf)++ = *s++;
    }
}

int vsprintf(char *buf, const char *fmt, va_list args) {
    char *p = buf;

    while (*fmt) {
        if (*fmt != '%') {
            *p++ = *fmt++;
            continue;
        }

        fmt++;

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
                if (is_longlong) v = va_arg(args, long long);
                else if (is_long) v = va_arg(args, long);
                else v = va_arg(args, int);

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
                if (is_longlong) v = va_arg(args, unsigned long long);
                else if (is_long) v = va_arg(args, unsigned long);
                else v = va_arg(args, unsigned int);

                char tmp[32];
                itoa(v, tmp, 10, 0);
                pad(&p, tmp, width, zero);
                break;
            }
            case 'x':
            case 'X': {
                unsigned long long v;
                if (is_longlong) v = va_arg(args, unsigned long long);
                else if (is_long) v = va_arg(args, unsigned long);
                else v = va_arg(args, unsigned int);

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
// Standard 3-bit ANSI Color Palette (ANSI 0-7) mapped to 32-bit ARGB
#define ANSI_COLOR_BLACK   0xFF000000  // 0
#define ANSI_COLOR_RED     0xFFCD0000  // 1
#define ANSI_COLOR_GREEN   0xFF00CD00  // 2
#define ANSI_COLOR_YELLOW  0xFFCDCD00  // 3
#define ANSI_COLOR_BLUE    0xFF0000EE  // 4
#define ANSI_COLOR_MAGENTA 0xFFCD00CD  // 5
#define ANSI_COLOR_CYAN    0xFF00CDCD  // 6
#define ANSI_COLOR_WHITE   0xFFE5E5E5  // 7

// A lookup array matching the 0-7 index structure
static const uint32_t ansi_palette[8] = {
    ANSI_COLOR_BLACK,
    ANSI_COLOR_RED,
    ANSI_COLOR_GREEN,
    ANSI_COLOR_YELLOW,
    ANSI_COLOR_BLUE,
    ANSI_COLOR_MAGENTA,
    ANSI_COLOR_CYAN,
    ANSI_COLOR_WHITE
};
void printk(LogType type, const char *fmt, ...)
{
    if (grad)
        return;

    char buf[1024];

    va_list args;
    uint64_t irq_flags = printk_irq_save();

    va_start(args, fmt);
    int len = vsprintf(buf, fmt, args);
    va_end(args);

    /* Log prefix */
    int color = 0;
    const char *text = " info   ";

    switch (type)
    {
        case LOG_DEBUG:   color = 4; text = " debug  "; break;
        case LOG_ERROR:   color = 1; text = " error  "; break;
        case LOG_WARNING: color = 3; text = " warning"; break;
        case LOG_ACPI:    color = 3; text = " uACPI  "; break;
        case LOG_TRACE:   color = 6; text = " trace  "; break;
        case LOG_NONE:    text = ""; break;
        default: break;
    }

    if (type != LOG_NONE)
    {
        tty_set_colors(ansi_palette[7], ansi_palette[color]);
        tty_write(text);
        tty_set_colors(ansi_palette[7], ansi_palette[0]);
        tty_putchar(' ');
    }

    uint32_t fg = ansi_palette[7];
    uint32_t bg = ansi_palette[0];
    bool bright = false;

    tty_set_colors(fg, bg);

    for (int i = 0; i < len;)
    {
        /* Serial always gets raw bytes */
        if (buf[i] == '\n')
            serial_write_char('\r');
        serial_write_char(buf[i]);

        /* ANSI escape? */
        if (buf[i] == '\033' && buf[i + 1] == '[')
        {
            i += 2;

            int value = 0;

            while (1)
            {
                value = 0;

                while (buf[i] >= '0' && buf[i] <= '9')
                {
                    value = value * 10 + (buf[i] - '0');
                    i++;
                }

                switch (value)
                {
                    case 0:
                        bright = false;
                        fg = ansi_palette[7];
                        bg = ansi_palette[0];
                        break;

                    case 1:
                        bright = true;
                        break;

                    case 22:
                        bright = false;
                        break;

                    /* Foreground */
                    case 30 ... 37:
                    {
                        int idx = value - 30;
                        fg = ansi_palette[idx];
                        break;
                    }

                    /* Background */
                    case 40 ... 47:
                    {
                        int idx = value - 40;
                        bg = ansi_palette[idx];
                        break;
                    }

                    /* Bright foreground */
                    case 90 ... 97:
                    {
                        int idx = value - 90;
                        fg = ansi_palette[idx];
                        bright = true;
                        break;
                    }

                    /* Bright background */
                    case 100 ... 107:
                    {
                        int idx = value - 100;
                        bg = ansi_palette[idx];
                        break;
                    }
                }

                if (buf[i] == ';')
                {
                    i++;
                    continue;
                }

                if (buf[i] == 'm')
                {
                    i++;
                    break;
                }

                break;
            }

            /* Optional bright effect */
            if (bright)
            {
                // Replace with your own bright palette if you have one.
            }

            tty_set_colors(fg, bg);
            continue;
        }

        tty_putchar(buf[i]);
        i++;
    }

    printk_irq_restore(irq_flags);
}

/**
 * @brief High-performance, optimized hardware rectangle rendering loop.
 */
void draw_rect(int rect_x, int rect_y, int rect_width, int rect_height, 
               uint8_t r, uint8_t g, uint8_t b) 
{
    if (!fb || !fb->address) return;

    // 1. Highly streamlined clipping boundaries
    int x1 = rect_x < 0 ? 0 : rect_x;
    int y1 = rect_y < 0 ? 0 : rect_y;
    int x2 = rect_x + rect_width;
    int x2_max = fb->width;
    int y2 = rect_y + rect_height;
    int y2_max = fb->height;

    if (x2 > x2_max) x2 = x2_max;
    if (y2 > y2_max) y2 = y2_max;
    if (x1 >= x2 || y1 >= y2) return;

    int fill_pixels = x2 - x1;
    int bytes_per_pixel = fb->bpp / 8;
    uint8_t* fb_bytes = (uint8_t*)fb->address;

    // Fast Path: 32-bit (4 bytes per pixel) layout (XRGB / ARGB / RGBA)
    if (bytes_per_pixel == 4) {
        // Pack into a single 32-bit register word.
        // Limine standard uses RGB ordering within fields; modify packing order if colors appear inverted.
        uint32_t color32 = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | (255U << 24);

        for (int y = y1; y < y2; y++) {
            uint32_t* row_ptr = (uint32_t*)(fb_bytes + (y * fb->pitch) + (x1 * 4));
            int count = fill_pixels;
            
            // Unrolled loop (4 pixels / 16 bytes per iteration)
            while (count >= 4) {
                row_ptr[0] = color32;
                row_ptr[1] = color32;
                row_ptr[2] = color32;
                row_ptr[3] = color32;
                row_ptr += 4;
                count -= 4;
            }
            // Clean remaining trailing pixels
            while (count > 0) {
                *row_ptr++ = color32;
                count--;
            }
        }
    } 
    // Alternate Path: 24-bit (3 bytes per pixel) packed layout
    else if (bytes_per_pixel == 3) {
        for (int y = y1; y < y2; y++) {
            uint8_t* row_ptr = fb_bytes + (y * fb->pitch) + (x1 * 3);
            int count = fill_pixels;

            // Unrolled loop (Fill 4 pixels across aligned 12 bytes blocks)
            while (count >= 4) {
                row_ptr[0] = r; row_ptr[1] = g; row_ptr[2] = b;
                row_ptr[3] = r; row_ptr[4] = g; row_ptr[5] = b;
                row_ptr[6] = r; row_ptr[7] = g; row_ptr[8] = b;
                row_ptr[9] = r; row_ptr[10] = g; row_ptr[11] = b;
                row_ptr += 12;
                count -= 4;
            }
            while (count > 0) {
                row_ptr[0] = r;
                row_ptr[1] = g;
                row_ptr[2] = b;
                row_ptr += 3;
                count--;
            }
        }
    }
}

void graduate() {
    grad = true;
    draw_rect(0, 0, fb->width, fb->height, 0, 0, 0);
}

void draw_image(int start_x, int start_y, int img_w, int img_h, const uint8_t *rgb_data) {
    if (!fb || !fb->address || !rgb_data) return;

    int src_x = 0;
    int src_y = 0;
    int dst_x1 = start_x < 0 ? 0 : start_x;
    int dst_y1 = start_y < 0 ? 0 : start_y;
    int dst_x2 = start_x + img_w;
    int dst_y2 = start_y + img_h;

    if (start_x < 0) src_x = -start_x;
    if (start_y < 0) src_y = -start_y;
    if (dst_x2 > (int)fb->width)  dst_x2 = fb->width;
    if (dst_y2 > (int)fb->height) dst_y2 = fb->height;

    if (dst_x1 >= dst_x2 || dst_y1 >= dst_y2) return;

    int fill_pixels = dst_x2 - dst_x1;
    int bytes_per_pixel = fb->bpp / 8;
    uint8_t *fb_bytes = (uint8_t *)fb->address;

    for (int y = dst_y1; y < dst_y2; y++) {
        uint8_t *dst_row = fb_bytes + (y * fb->pitch) + (dst_x1 * bytes_per_pixel);
        const uint8_t *src_row = rgb_data + ((src_y + (y - dst_y1)) * img_w * 3) + (src_x * 3);

        if (bytes_per_pixel == 3) {
            memcpy(dst_row, src_row, fill_pixels * 3);
        } 
        else if (bytes_per_pixel == 4) {
            uint32_t *dst_ptr32 = (uint32_t *)dst_row;
            int count = fill_pixels;
            
            while (count >= 4) {
                dst_ptr32[0] = src_row[0] | (src_row[1] << 8) | (src_row[2] << 16) | (255U << 24);
                dst_ptr32[1] = src_row[3] | (src_row[4] << 8) | (src_row[5] << 16) | (255U << 24);
                dst_ptr32[2] = src_row[6] | (src_row[7] << 8) | (src_row[8] << 16) | (255U << 24);
                dst_ptr32[3] = src_row[9] | (src_row[10] << 8) | (src_row[11] << 16) | (255U << 24);
                dst_ptr32 += 4;
                src_row += 12;
                count -= 4;
            }
            while (count > 0) {
                *dst_ptr32++ = src_row[0] | (src_row[1] << 8) | (src_row[2] << 16) | (255U << 24);
                src_row += 3;
                count--;
            }
        }
    }
}