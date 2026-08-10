#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <fs/mnt.h>
#include "font.h"
#include <drivers/tty.h>
#include <hals/virtio/virtio_gpu.h>

#define BIT(x) (1ULL << (x))

// Standard terminal IOCTL definitions
#define TCGETS          0x5401
#define TCSETS          0x5402
#define TCSETSW         0x5403  
#define TCSETSF         0x5404  
#define TIOCGWINSZ      0x5413  
#define TIOCSWINSZ      0x5414  
#define TIOCFLUSH       0x540B  
#define TIOCGPGRP       0x540F  
#define TIOCSPGRP       0x5410  
#define TIOCSPTLCK      0x40045431 
#define TIOCGPTN        0x80045430 

#define TTY_MAX_COLS    256
#define TTY_MAX_ROWS    128
#define MAX_PTYS        32         

#define TTY7_MAX_FB_WIDTH   1920
#define TTY7_MAX_FB_HEIGHT  1080
#define TTY7_MAX_FB_SIZE    (TTY7_MAX_FB_WIDTH * TTY7_MAX_FB_HEIGHT * sizeof(uint32_t))

#define PSF1_MAGIC0     0x36
#define PSF1_MAGIC1     0x04
#define PSF1_MODE512    0x01

extern char kgetc(void);
int echo = 1;

// Global VirtIO GPU device reference
extern virtio_gpu_device_t g_virtio_gpu;

typedef struct {
    unsigned char magic[2];
    unsigned char mode;
    unsigned char charsize;
} __attribute__((packed)) psf1_header_t;

extern unsigned char Lat2_Terminus16_psf[];

static const uint32_t ansi_palette[16] = {
    0xFF000000, 0xFFAA0000, 0xFF00AA00, 0xFFAAAA00,
    0xFF0000AA, 0xFFAA00AA, 0xFF00AAAA, 0xFFAAAAAA,
    0xFF555555, 0xFFFF5555, 0xFF55FF55, 0xFFFFFF55,
    0xFF5555FF, 0xFFFF55FF, 0xFF55FFFF, 0xFFFFFFFF
};

static framebuffer_t global_fb;
static int fb_initialized = 0;
static tty_t ttys[8];
static uint32_t active_tty_idx = 1;
static uint32_t font_height = 16;
static const uint32_t font_width = 8;
bool gpu = false;

// Dirty Tracking State for Batch VirtIO Updates
static uint32_t dirty_min_x = UINT32_MAX;
static uint32_t dirty_min_y = UINT32_MAX;
static uint32_t dirty_max_x = 0;
static uint32_t dirty_max_y = 0;

// TTY7 Pixel Backing Store Buffer
static uint32_t tty7_backing_store[TTY7_MAX_FB_WIDTH * TTY7_MAX_FB_HEIGHT];
static size_t tty7_write_offset = 0;
static size_t tty7_read_offset = 0;

#define TTY_BUF_SIZE 1024
static char input_buffers[8][TTY_BUF_SIZE];
static uint32_t input_head[8] = {0};
static uint32_t input_tail[8] = {0};

static tty_cell_t static_grid_pool[8][TTY_MAX_ROWS * TTY_MAX_COLS];
static int32_t tty_pgrps[8] = {0};

typedef struct {
    char buf[TTY_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
} pty_ringbuf_t;

typedef struct {
    bool allocated;
    bool locked;
    struct termios term;
    struct winsize winsz;
    pty_ringbuf_t controller_to_replica;
    pty_ringbuf_t replica_to_controller;
    int32_t pgrp;
} pty_pair_t;

static pty_pair_t pty_pairs[MAX_PTYS];

// Forward declarations
size_t tty_read(int fd, void *buf, size_t count, int offset);
size_t tty_write_dev(int fd, const void *buf, size_t count);
int tty_ioctl(int fd, unsigned long request, void *arg);
void tty_putchar_to(uint32_t tty_idx, char c);
static void tty_draw_cursor(uint32_t tty_idx, bool show);
static void tty_redraw_active(void);

// --- Dirty Region Helper Functions ---
static inline void mark_dirty_region(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (x < dirty_min_x) dirty_min_x = x;
    if (y < dirty_min_y) dirty_min_y = y;
    if (x + w > dirty_max_x) dirty_max_x = x + w;
    if (y + h > dirty_max_y) dirty_max_y = y + h;
}

static inline void mark_dirty_full(void) {
    dirty_min_x = 0;
    dirty_min_y = 0;
    dirty_max_x = global_fb.width;
    dirty_max_y = global_fb.height;
}

static void tty_flush_dirty(void) {
    if (!gpu || dirty_min_x >= dirty_max_x || dirty_min_y >= dirty_max_y) return;

    if (dirty_max_x > global_fb.width) dirty_max_x = global_fb.width;
    if (dirty_max_y > global_fb.height) dirty_max_y = global_fb.height;

    uint32_t w = dirty_max_x - dirty_min_x;
    uint32_t h = dirty_max_y - dirty_min_y;

    virtio_gpu_flush(&g_virtio_gpu, dirty_min_x, dirty_min_y, w, h);

    // Reset bounding box
    dirty_min_x = UINT32_MAX;
    dirty_min_y = UINT32_MAX;
    dirty_max_x = 0;
    dirty_max_y = 0;
}

static void pty_putc(pty_ringbuf_t *rb, char c) {
    uint32_t next = (rb->head + 1) % TTY_BUF_SIZE;
    if (next != rb->tail) {
        rb->buf[rb->head] = c;
        rb->head = next;
    }
}

static char pty_getc(pty_ringbuf_t *rb) {
    if (rb->head == rb->tail) return 0;
    char c = rb->buf[rb->tail];
    rb->tail = (rb->tail + 1) % TTY_BUF_SIZE;
    return c;
}

static bool pty_has_data(pty_ringbuf_t *rb) {
    return rb->head != rb->tail;
}

// --- Core Pixel & Rectangle Rendering ---
void tty_draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_initialized) return;
    if (x >= global_fb.width || y >= global_fb.height) return;

    tty7_backing_store[y * global_fb.width + x] = color;

    if (gpu && g_virtio_gpu.framebuffer) {
        g_virtio_gpu.framebuffer[y * global_fb.width + x] = color;
        mark_dirty_region(x, y, 1, 1);
    } else if (active_tty_idx == 7 && global_fb.address) {
        uint32_t stride = global_fb.pitch / sizeof(uint32_t);
        global_fb.address[y * stride + x] = color;
    }
}

void tty_draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (!fb_initialized) return;

    uint32_t *target_buf = (gpu && g_virtio_gpu.framebuffer) ? g_virtio_gpu.framebuffer : global_fb.address;
    if (!target_buf) return;

    uint32_t pitch = (gpu) ? global_fb.width : (global_fb.pitch / sizeof(uint32_t));

    for (int cy = y; cy < y + h && cy < (int)global_fb.height; cy++) {
        if (cy < 0) continue;
        for (int cx = x; cx < x + w && cx < (int)global_fb.width; cx++) {
            if (cx < 0) continue;
            target_buf[cy * pitch + cx] = color;
        }
    }

    if (gpu) {
        mark_dirty_region(x, y, w, h);
    }
}

void tty_switch_gpu(void) {
    gpu = true;
    tty_draw_rect(0, 0, global_fb.width, global_fb.height, 0xFF000000);
    tty_flush_dirty();
}

// --- Software In-Memory PSF1 Character Renderer ---
void tty_draw_char_psf1(framebuffer_t *fb, char c, int cx, int cy, uint32_t fg_color, uint32_t bg_color) {
    if (!fb_initialized) return;
    psf1_header_t *font = (psf1_header_t *)Lat2_Terminus16_psf;
    if (font->magic[0] != PSF1_MAGIC0 || font->magic[1] != PSF1_MAGIC1) return;

    int num_glyphs = (font->mode & PSF1_MODE512) ? 512 : 256;
    unsigned char *glyph = (unsigned char *)Lat2_Terminus16_psf +
                           sizeof(psf1_header_t) +
                           ((unsigned char)c < num_glyphs ? (unsigned char)c : 0) * font->charsize;

    uint32_t *dest = (gpu && g_virtio_gpu.framebuffer) ? g_virtio_gpu.framebuffer : fb->address;
    if (!dest) return;

    uint32_t stride = gpu ? global_fb.width : (fb->pitch / sizeof(uint32_t));

    for (uint32_t y = 0; y < font->charsize; y++) {
        uint32_t sy = cy + y;
        if (sy >= global_fb.height) break;

        unsigned char row_byte = glyph[y];
        uint32_t row_offset = sy * stride;

        for (uint32_t x = 0; x < font_width; x++) {
            uint32_t sx = cx + x;
            if (sx >= global_fb.width) break;

            dest[row_offset + sx] = (row_byte & (0x80 >> x)) ? fg_color : bg_color;
        }
    }

    if (gpu) {
        mark_dirty_region(cx, cy, font_width, font->charsize);
    }
}

static void tty_draw_cursor(uint32_t tty_idx, bool show) {
    if (tty_idx == 7 || !fb_initialized || tty_idx != active_tty_idx) return;
    tty_t *tty = &ttys[tty_idx];
    if (!tty->grid || !tty->cursor_enabled) return;

    uint32_t col = tty->cursor_x;
    uint32_t row = tty->cursor_y;
    if (col >= tty->cols || row >= tty->rows) return;

    tty_cell_t cell = tty->grid[row * tty->cols + col];
    if (show) {
        tty_draw_char_psf1(&global_fb, cell.ch, col * font_width, row * font_height, cell.bg, cell.fg);
    } else {
        tty_draw_char_psf1(&global_fb, cell.ch, col * font_width, row * font_height, cell.fg, cell.bg);
    }
}

static void tty_flush_cell(uint32_t tty_idx, uint32_t col, uint32_t row) {
    if (tty_idx == 7 || !fb_initialized || tty_idx != active_tty_idx) return;
    tty_t *tty = &ttys[tty_idx];
    if (!tty->grid || col >= tty->cols || row >= tty->rows) return;

    tty_cell_t cell = tty->grid[row * tty->cols + col];
    tty_draw_char_psf1(&global_fb, cell.ch, col * font_width, row * font_height, cell.fg, cell.bg);
}

static void tty_redraw_active(void) {
    if (!fb_initialized) return;

    if (active_tty_idx == 7) {
        uint32_t *dest = (gpu && g_virtio_gpu.framebuffer) ? g_virtio_gpu.framebuffer : global_fb.address;
        if (dest) {
            size_t stride = gpu ? global_fb.width : (global_fb.pitch / sizeof(uint32_t));
            for (uint32_t y = 0; y < global_fb.height; y++) {
                for (uint32_t x = 0; x < global_fb.width; x++) {
                    dest[y * stride + x] = tty7_backing_store[y * global_fb.width + x];
                }
            }
        }
        mark_dirty_full();
        tty_flush_dirty();
        return;
    }

    tty_t *tty = &ttys[active_tty_idx];
    if (!tty->grid) return;

    for (uint32_t r = 0; r < tty->rows; r++) {
        for (uint32_t c = 0; c < tty->cols; c++) {
            tty_flush_cell(active_tty_idx, c, r);
        }
    }

    if (tty->cursor_enabled && tty->cursor_draw_state) {
        tty_draw_cursor(active_tty_idx, true);
    }

    tty_flush_dirty();
}

static void tty_scroll(uint32_t tty_idx) {
    if (tty_idx == 7) return;
    tty_t *tty = &ttys[tty_idx];
    if (!tty->grid) return;

    uint32_t top = tty->scroll_top;
    uint32_t bottom = tty->scroll_bottom;
    if (top >= bottom || bottom >= tty->rows) return;

    if (tty_idx == active_tty_idx && tty->cursor_draw_state) {
        tty_draw_cursor(tty_idx, false);
    }

    // 1. Loop-based Grid Row Shifting (No memmove dependency)
    for (uint32_t r = top; r < bottom; r++) {
        uint32_t dst_offset = r * tty->cols;
        uint32_t src_offset = (r + 1) * tty->cols;
        for (uint32_t c = 0; c < tty->cols; c++) {
            tty->grid[dst_offset + c] = tty->grid[src_offset + c];
        }
    }

    // 2. Clear Last Row in Cell Grid
    uint32_t last_row_offset = bottom * tty->cols;
    for (uint32_t c = 0; c < tty->cols; c++) {
        tty->grid[last_row_offset + c].ch = ' ';
        tty->grid[last_row_offset + c].fg = tty->current_fg;
        tty->grid[last_row_offset + c].bg = tty->current_bg;
    }

    // 3. Full repaint sync on screen
    if (tty_idx == active_tty_idx) {
        tty_redraw_active();
    }
}

static void tty_resize(uint32_t tty_idx, uint16_t new_cols, uint16_t new_rows) {
    if (tty_idx == 7) return;
    tty_t *tty = &ttys[tty_idx];

    if (new_cols > TTY_MAX_COLS) new_cols = TTY_MAX_COLS;
    if (new_rows > TTY_MAX_ROWS) new_rows = TTY_MAX_ROWS;
    if (new_cols == 0) new_cols = 1;
    if (new_rows == 0) new_rows = 1;

    if (tty->cols == new_cols && tty->rows == new_rows) return;

    if (tty_idx == active_tty_idx && tty->cursor_draw_state) {
        tty_draw_cursor(tty_idx, false);
    }

    tty_cell_t temp_grid[TTY_MAX_ROWS * TTY_MAX_COLS];
    for (uint32_t i = 0; i < tty->rows * tty->cols; i++) {
        temp_grid[i] = tty->grid[i];
    }

    uint32_t old_cols = tty->cols;
    uint32_t old_rows = tty->rows;

    tty->cols = new_cols;
    tty->rows = new_rows;
    tty->scroll_top = 0;
    tty->scroll_bottom = new_rows - 1;

    if (tty->cursor_x >= tty->cols) tty->cursor_x = tty->cols - 1;
    if (tty->cursor_y >= tty->rows) tty->cursor_y = tty->rows - 1;

    for (uint32_t r = 0; r < new_rows; r++) {
        for (uint32_t c = 0; c < new_cols; c++) {
            uint32_t new_offset = r * new_cols + c;
            if (r < old_rows && c < old_cols) {
                tty->grid[new_offset] = temp_grid[r * old_cols + c];
            } else {
                tty->grid[new_offset].ch = ' ';
                tty->grid[new_offset].fg = tty->default_fg;
                tty->grid[new_offset].bg = tty->default_bg;
            }
        }
    }

    if (tty_idx == active_tty_idx) {
        tty_redraw_active();
    }
}

static uint32_t parse_256_color(uint8_t index) {
    if (index < 16) return ansi_palette[index];
    if (index < 232) {
        uint8_t r = (index - 16) / 36;
        uint8_t g = ((index - 16) % 36) / 6;
        uint8_t b = (index - 16) % 6;
        return 0xFF000000 | ((r * 51) << 16) | ((g * 51) << 8) | (b * 51);
    }
    uint8_t gray = 8 + (index - 232) * 10;
    return 0xFF000000 | (gray << 16) | (gray << 8) | gray;
}

static void tty_ansi_execute_csi(tty_t *tty, char command) {
    uint32_t p1 = (tty->ansi_param_count > 0) ? tty->ansi_params[0] : 0;
    uint32_t p2 = (tty->ansi_param_count > 1) ? tty->ansi_params[1] : 0;

    switch (command) {
        case 'H':
        case 'f': {
            uint32_t row = (p1 == 0) ? 1 : p1;
            uint32_t col = (p2 == 0) ? 1 : p2;
            tty->cursor_y = (row > tty->rows) ? tty->rows - 1 : row - 1;
            tty->cursor_x = (col > tty->cols) ? tty->cols - 1 : col - 1;
            break;
        }
        case 'A': {
            uint32_t move = (p1 == 0) ? 1 : p1;
            tty->cursor_y = (tty->cursor_y >= move) ? tty->cursor_y - move : 0;
            break;
        }
        case 'B': {
            uint32_t move = (p1 == 0) ? 1 : p1;
            tty->cursor_y += move;
            if (tty->cursor_y >= tty->rows) tty->cursor_y = tty->rows - 1;
            break;
        }
        case 'C': {
            uint32_t move = (p1 == 0) ? 1 : p1;
            tty->cursor_x += move;
            if (tty->cursor_x >= tty->cols) tty->cursor_x = tty->cols - 1;
            break;
        }
        case 'D': {
            uint32_t move = (p1 == 0) ? 1 : p1;
            tty->cursor_x = (tty->cursor_x >= move) ? tty->cursor_x - move : 0;
            break;
        }
        case 'J': {
            uint32_t start = 0, end = tty->rows * tty->cols;
            if (p1 == 0) start = tty->cursor_y * tty->cols + tty->cursor_x;
            else if (p1 == 1) end = (tty->cursor_y * tty->cols + tty->cursor_x) + 1;

            for (uint32_t idx = start; idx < end; idx++) {
                tty->grid[idx].ch = ' ';
                tty->grid[idx].fg = tty->current_fg;
                tty->grid[idx].bg = tty->current_bg;
            }

            if (p1 == 2 || p1 == 0) {
                tty->cursor_x = 0;
                tty->cursor_y = 0;
            }

            if (tty - ttys == active_tty_idx) tty_redraw_active();
            break;
        }
        case 'K': {
            uint32_t start = 0, end = tty->cols;
            if (p1 == 0) start = tty->cursor_x;
            else if (p1 == 1) end = tty->cursor_x + 1;

            uint32_t offset = tty->cursor_y * tty->cols;
            for (uint32_t col = start; col < end; col++) {
                tty->grid[offset + col].ch = ' ';
                tty->grid[offset + col].fg = tty->current_fg;
                tty->grid[offset + col].bg = tty->current_bg;
                tty_flush_cell(tty - ttys, col, tty->cursor_y);
            }
            break;
        }
        case 's': tty->saved_x = tty->cursor_x; tty->saved_y = tty->cursor_y; break;
        case 'u': tty->cursor_x = tty->saved_x; tty->cursor_y = tty->saved_y; break;
        case 'r': {
            uint32_t top = (p1 == 0) ? 1 : p1;
            uint32_t bottom = (p2 == 0) ? tty->rows : p2;
            if (top > 0 && bottom <= tty->rows && top < bottom) {
                tty->scroll_top = top - 1;
                tty->scroll_bottom = bottom - 1;
            }
            tty->cursor_x = 0;
            tty->cursor_y = tty->scroll_top;
            break;
        }
        case 'm': {
            uint32_t i = 0;
            if (tty->ansi_param_count == 0) {
                tty->ansi_params[0] = 0;
                tty->ansi_param_count = 1;
            }
            while (i < tty->ansi_param_count) {
                uint32_t param = tty->ansi_params[i];
                switch (param) {
                    case 0: tty->current_fg = tty->default_fg; tty->current_bg = tty->default_bg; tty->bold = false; tty->underline = false; i++; break;
                    case 1: {
                        tty->bold = true;
                        for (int p = 0; p < 8; p++) {
                            if (tty->current_fg == ansi_palette[p]) { tty->current_fg = ansi_palette[p + 8]; break; }
                        }
                        i++; break;
                    }
                    case 4: tty->underline = true; i++; break;
                    case 22: tty->bold = false; i++; break;
                    case 24: tty->underline = false; i++; break;
                    case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37:
                        tty->current_fg = ansi_palette[(param - 30) + (tty->bold ? 8 : 0)]; i++; break;
                    case 38: {
                        if (i + 2 < tty->ansi_param_count) {
                            uint32_t mode = tty->ansi_params[i + 1];
                            if (mode == 5) { tty->current_fg = parse_256_color(tty->ansi_params[i + 2]); i += 3; }
                            else if (mode == 2 && i + 4 < tty->ansi_param_count) {
                                tty->current_fg = 0xFF000000 | (tty->ansi_params[i+2] << 16) | (tty->ansi_params[i+3] << 8) | tty->ansi_params[i+4];
                                i += 5;
                            } else i++;
                        } else i++;
                        break;
                    }
                    case 39: tty->current_fg = tty->default_fg; i++; break;
                    case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
                        tty->current_bg = ansi_palette[(param - 40)]; i++; break;
                    case 48: {
                        if (i + 2 < tty->ansi_param_count) {
                            uint32_t mode = tty->ansi_params[i + 1];
                            if (mode == 5) { tty->current_bg = parse_256_color(tty->ansi_params[i + 2]); i += 3; }
                            else if (mode == 2 && i + 4 < tty->ansi_param_count) {
                                tty->current_bg = 0xFF000000 | (tty->ansi_params[i+2] << 16) | (tty->ansi_params[i+3] << 8) | tty->ansi_params[i+4];
                                i += 5;
                            } else i++;
                        } else i++;
                        break;
                    }
                    case 49: tty->current_bg = tty->default_bg; i++; break;
                    case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97:
                        tty->current_fg = ansi_palette[(param - 90) + 8]; i++; break;
                    case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107:
                        tty->current_bg = ansi_palette[(param - 100) + 8]; i++; break;
                    default: i++; break;
                }
            }
            break;
        }
        default: break;
    }
}

void tty_set_colors_to(uint32_t tty_idx, uint32_t fg, uint32_t bg) {
    if (tty_idx >= 8) return;
    ttys[tty_idx].default_fg = fg;
    ttys[tty_idx].default_bg = bg;
    ttys[tty_idx].current_fg = fg;
    ttys[tty_idx].current_bg = bg;
}

void tty_set_colors(uint32_t fg, uint32_t bg) {
    tty_set_colors_to(active_tty_idx, fg, bg);
}

void tty_switch(uint32_t tty_idx) {
    if (tty_idx >= 8 || tty_idx == active_tty_idx) return;

    if (ttys[active_tty_idx].cursor_draw_state) {
        tty_draw_cursor(active_tty_idx, false);
    }

    active_tty_idx = tty_idx;

    // Direct background clear when switching screens
    if (active_tty_idx != 7) {
        tty_draw_rect(0, 0, global_fb.width, global_fb.height, ttys[active_tty_idx].default_bg);
    } else {
        tty_draw_rect(0, 0, global_fb.width, global_fb.height, 0xFF000000);
    }

    tty_redraw_active();
    tty_flush_dirty();
}

uint32_t tty_get_active(void) {
    return active_tty_idx;
}

void tty_putchar_to(uint32_t tty_idx, char c) {
    if (tty_idx == 7 || tty_idx >= 8) return;
    tty_t *tty = &ttys[tty_idx];
    if (!tty->grid) return;

    if (tty_idx == active_tty_idx && tty->cursor_draw_state) {
        tty_draw_cursor(tty_idx, false);
    }

    // ANSI Parsing logic
    if (tty->ansi_state == ANSI_STATE_ESC) {
        if (c == '[') {
            tty->ansi_state = ANSI_STATE_CSI;
            tty->ansi_param_count = 0;
            tty->ansi_params[0] = 0;
            tty->ansi_has_param = false;
        } else if (c == '7') {
            tty->saved_x = tty->cursor_x; tty->saved_y = tty->cursor_y;
            tty->ansi_state = ANSI_STATE_NORMAL;
        } else if (c == '8') {
            tty->cursor_x = tty->saved_x; tty->cursor_y = tty->saved_y;
            tty->ansi_state = ANSI_STATE_NORMAL;
        } else {
            tty->ansi_state = ANSI_STATE_NORMAL;
            tty_putchar_to(tty_idx, '\x1B');
            tty_putchar_to(tty_idx, c);
        }
        goto draw_cursor_after_write;
    }
    else if (tty->ansi_state == ANSI_STATE_CSI) {
        if (c >= '0' && c <= '9') {
            if (tty->ansi_param_count < ANSI_MAX_PARAMS) {
                tty->ansi_params[tty->ansi_param_count] = (tty->ansi_params[tty->ansi_param_count] * 10) + (c - '0');
                tty->ansi_has_param = true;
            }
            goto draw_cursor_after_write;
        }
        else if (c == ';') {
            if (tty->ansi_has_param) {
                tty->ansi_param_count++;
                if (tty->ansi_param_count < ANSI_MAX_PARAMS) tty->ansi_params[tty->ansi_param_count] = 0;
                tty->ansi_has_param = false;
            } else {
                if (tty->ansi_param_count < ANSI_MAX_PARAMS) {
                    tty->ansi_params[tty->ansi_param_count] = 0;
                    tty->ansi_param_count++;
                    if (tty->ansi_param_count < ANSI_MAX_PARAMS) tty->ansi_params[tty->ansi_param_count] = 0;
                }
            }
            return;
        }
        else {
            if (tty->ansi_has_param) tty->ansi_param_count++;
            tty_ansi_execute_csi(tty, c);
            tty->ansi_state = ANSI_STATE_NORMAL;
            goto draw_cursor_after_write;
        }
    }

    if (c == '\x1B') {
        tty->ansi_state = ANSI_STATE_ESC;
        goto draw_cursor_after_write;
    }

    // Control Characters
    if (c == '\n') {
        tty->cursor_x = 0;
        tty->cursor_y++;
    }
    else if (c == '\r') {
        tty->cursor_x = 0;
    }
    else if (c == '\b') {
        if (tty->cursor_x > 0) {
            tty->cursor_x--;
        } else if (tty->cursor_y > tty->scroll_top) {
            tty->cursor_y--;
            tty->cursor_x = tty->cols - 1;
        } else {
            goto draw_cursor_after_write;
        }

        uint32_t offset = tty->cursor_y * tty->cols + tty->cursor_x;
        tty->grid[offset].ch = ' ';
        tty->grid[offset].fg = tty->current_fg;
        tty->grid[offset].bg = tty->current_bg;
        tty_flush_cell(tty_idx, tty->cursor_x, tty->cursor_y);
    }
    else if (c == '\t') {
        uint32_t next_tab = (tty->cursor_x + 4) & ~3;
        while (tty->cursor_x < next_tab && tty->cursor_x < tty->cols) {
            tty_putchar_to(tty_idx, ' ');
        }
    }
    else {
        // Printable Characters
        if (tty->cursor_x < tty->cols && tty->cursor_y < tty->rows) {
            uint32_t offset = tty->cursor_y * tty->cols + tty->cursor_x;
            tty->grid[offset].ch = c;
            tty->grid[offset].fg = tty->current_fg;
            tty->grid[offset].bg = tty->current_bg;
            tty_flush_cell(tty_idx, tty->cursor_x, tty->cursor_y);
            tty->cursor_x++;
        }
    }

    // Line wrap logic
    if (tty->cursor_x >= tty->cols) {
        tty->cursor_x = 0;
        tty->cursor_y++;
    }

    // Scroll check
    while (tty->cursor_y > tty->scroll_bottom) {
        tty->cursor_y--;
        tty_scroll(tty_idx);
    }

draw_cursor_after_write:
    tty->cursor_draw_state = true;
    tty->cursor_tick = 0;
    if (tty_idx == active_tty_idx) {
        tty_draw_cursor(tty_idx, true);
    }
}

void tty_putchar_active(char c) {
    tty_putchar_to(active_tty_idx, c);
    tty_flush_dirty();
}

void tty_putchar(char c) {
    tty_putchar_to(active_tty_idx, c);
    tty_flush_dirty();
}

void tty_write(const char *str) {
    while (*str) tty_putchar_to(active_tty_idx, *str++);
    tty_flush_dirty();
}

void tty_write_to(uint32_t tty_idx, const char *str) {
    while (*str) tty_putchar_to(tty_idx, *str++);
    if (tty_idx == active_tty_idx) tty_flush_dirty();
}

void tty_handle_input(char c) {
    if (c >= ' ' && c <= '~') {
        uint32_t tty_idx = active_tty_idx;
        uint32_t next = (input_head[tty_idx] + 1) % TTY_BUF_SIZE;

        if (next != input_tail[tty_idx]) {
            input_buffers[tty_idx][input_head[tty_idx]] = c;
            input_head[tty_idx] = next;
        }

        if (echo_is_on()) {
            tty_putchar_active(c);
        }
    }
}

size_t tty_read(int fd, void *buf, size_t count, int offset) {
    if (!buf || count == 0) return 0;

    if (fd >= 100 && fd < 200) {
        int pty_idx = fd - 100;
        pty_pair_t *pair = &pty_pairs[pty_idx];
        if (!pair->allocated) return 0;

        size_t read_bytes = 0;
        char *out = (char *)buf;
        while (read_bytes < count && pty_has_data(&pair->replica_to_controller)) {
            out[read_bytes++] = pty_getc(&pair->replica_to_controller);
        }
        return read_bytes;
    }
    else if (fd >= 200 && fd < 300) {
        int pty_idx = fd - 200;
        pty_pair_t *pair = &pty_pairs[pty_idx];
        if (!pair->allocated || pair->locked) return 0;

        size_t read_bytes = 0;
        char *out = (char *)buf;
        bool canonical = (pair->term.c_lflag & ICANON) != 0;

        while (read_bytes < count && pty_has_data(&pair->controller_to_replica)) {
            char c = pty_getc(&pair->controller_to_replica);
            if (canonical && (c == '\r' || c == '\n')) {
                out[read_bytes++] = '\n';
                break;
            }
            out[read_bytes++] = c;
        }
        return read_bytes;
    }

    if (active_tty_idx == 7) {
        if (!fb_initialized) return 0;

        size_t total_fb_bytes = global_fb.height * global_fb.width * sizeof(uint32_t);
        size_t read_ptr = (offset > 0) ? (size_t)offset : tty7_read_offset;

        if (read_ptr >= total_fb_bytes) {
            if (offset <= 0) tty7_read_offset = 0;
            return 0;
        }

        if (read_ptr + count > total_fb_bytes) {
            count = total_fb_bytes - read_ptr;
        }

        uint8_t *bs_byte_ptr = (uint8_t *)tty7_backing_store;
        for (size_t i = 0; i < count; i++) {
            ((uint8_t *)buf)[i] = bs_byte_ptr[read_ptr + i];
        }

        if (offset <= 0) {
            tty7_read_offset = (read_ptr + count) % total_fb_bytes;
        }
        return count;
    }

    char *char_buf = (char *)buf;
    size_t bytes_read = 0;
    uint32_t tty_idx = active_tty_idx;
    tty_t *tty = &ttys[tty_idx];
    bool canonical = (tty->term.c_lflag & ICANON) != 0;
    bool do_echo = (tty->term.c_lflag & ECHO) && echo;

    while (bytes_read < count) {
        if (input_head[tty_idx] == input_tail[tty_idx]) {
            char incoming = kgetc();
            tty_handle_input(incoming);
            if (input_head[tty_idx] == input_tail[tty_idx]) {
                continue;
            }
        }

        char c = input_buffers[tty_idx][input_tail[tty_idx]];
        input_tail[tty_idx] = (input_tail[tty_idx] + 1) % TTY_BUF_SIZE;

        if (canonical) {
            if (c == '\n' || c == '\r') {
                if (do_echo) tty_putchar_active('\n');
                char_buf[bytes_read++] = '\n';
                break;
            }
            if (c == '\b') {
                if (bytes_read > 0) {
                    bytes_read--;
                    if (do_echo) tty_putchar_active('\b');
                }
                continue;
            }
            if (c >= ' ' && c <= '~') {
                char_buf[bytes_read++] = c;
            }
        } else {
            char_buf[bytes_read++] = c;
            break;
        }
    }
    return bytes_read;
}

size_t tty_write_dev(int fd, const void *buf, size_t count) {
    if (!buf || count == 0) return 0;

    if (fd >= 100 && fd < 200) {
        int pty_idx = fd - 100;
        pty_pair_t *pair = &pty_pairs[pty_idx];
        if (!pair->allocated) return 0;

        const char *src = (const char *)buf;
        for (size_t i = 0; i < count; i++) {
            pty_putc(&pair->controller_to_replica, src[i]);
        }
        return count;
    }
    else if (fd >= 200 && fd < 300) {
        int pty_idx = fd - 200;
        pty_pair_t *pair = &pty_pairs[pty_idx];
        if (!pair->allocated || pair->locked) return 0;

        const char *src = (const char *)buf;
        bool do_echo = (pair->term.c_lflag & ECHO);

        for (size_t i = 0; i < count; i++) {
            pty_putc(&pair->replica_to_controller, src[i]);
            if (do_echo) {
                pty_putc(&pair->controller_to_replica, src[i]);
            }
        }
        return count;
    }

    if (active_tty_idx == 7) {
        if (!fb_initialized) return 0;

        size_t total_fb_bytes = global_fb.height * global_fb.width * sizeof(uint32_t);
        uint8_t *bs_byte_ptr = (uint8_t *)tty7_backing_store;
        uint8_t *gpu_byte_ptr = (uint8_t *)g_virtio_gpu.framebuffer;
        const uint8_t *src_pixels = (const uint8_t *)buf;

        size_t start_offset = tty7_write_offset;

        for (size_t i = 0; i < count; i++) {
            bs_byte_ptr[tty7_write_offset] = src_pixels[i];
            if (gpu && gpu_byte_ptr) {
                gpu_byte_ptr[tty7_write_offset] = src_pixels[i];
            } else if (global_fb.address) {
                ((uint8_t *)global_fb.address)[tty7_write_offset] = src_pixels[i];
            }

            tty7_write_offset++;
            if (tty7_write_offset >= total_fb_bytes) {
                tty7_write_offset = 0;
            }
        }

        if (gpu) {
            uint32_t start_pixel = start_offset / sizeof(uint32_t);
            uint32_t end_pixel = (start_offset + count) / sizeof(uint32_t);
            uint32_t start_y = start_pixel / global_fb.width;
            uint32_t end_y = (end_pixel / global_fb.width) + 1;

            if (end_y > global_fb.height) end_y = global_fb.height;
            mark_dirty_region(0, start_y, global_fb.width, end_y - start_y);
            tty_flush_dirty();
        }
        return count;
    }

    const char *char_buf = (const char *)buf;
    for (size_t i = 0; i < count; i++) {
        tty_putchar_to(active_tty_idx, char_buf[i]);
    }

    tty_flush_dirty();
    return count;
}

int tty_ioctl(int fd, unsigned long request, void *arg) {
    if (fd == 99 || (fd >= 100 && fd < 200)) {
        int pty_idx = (fd == 99) ? 0 : (fd - 100);

        if (request == TIOCGPTN) {
            for (int i = 0; i < MAX_PTYS; i++) {
                if (!pty_pairs[i].allocated) {
                    pty_pairs[i].allocated = true;
                    pty_pairs[i].locked = true;
                    pty_pairs[i].term.c_lflag = ECHO | ICANON;
                    *(int *)arg = i;
                    return 0;
                }
            }
            return -1;
        }
        if (request == TIOCSPTLCK) {
            pty_pairs[pty_idx].locked = (*(int *)arg != 0);
            return 0;
        }
        return -1;
    }

    if (fd >= 200 && fd < 300) {
        int pty_idx = fd - 200;
        pty_pair_t *pair = &pty_pairs[pty_idx];
        if (!pair->allocated) return -1;

        switch (request) {
            case TCGETS:
                for (size_t i = 0; i < sizeof(struct termios); i++) {
                    ((char *)arg)[i] = ((char *)&pair->term)[i];
                }
                return 0;
            case TCSETS:
            case TCSETSW:
            case TCSETSF:
                for (size_t i = 0; i < sizeof(struct termios); i++) {
                    ((char *)&pair->term)[i] = ((char *)arg)[i];
                }
                return 0;
            case TIOCGWINSZ:
                for (size_t i = 0; i < sizeof(struct winsize); i++) {
                    ((char *)arg)[i] = ((char *)&pair->winsz)[i];
                }
                return 0;
            case TIOCSWINSZ:
                for (size_t i = 0; i < sizeof(struct winsize); i++) {
                    ((char *)&pair->winsz)[i] = ((char *)arg)[i];
                }
                return 0;
            case TIOCGPGRP: *(int32_t *)arg = pair->pgrp; return 0;
            case TIOCSPGRP: pair->pgrp = *(int32_t *)arg; return 0;
            default: return -1;
        }
    }

    uint32_t tty_idx = active_tty_idx;
    tty_t *tty = &ttys[tty_idx];

    if (tty_idx == 7) {
        if (request == TIOCGWINSZ && arg) {
            struct winsize *ws = (struct winsize *)arg;
            ws->ws_row = 0;
            ws->ws_col = 0;
            ws->ws_xpixel = global_fb.width;
            ws->ws_ypixel = global_fb.height;
            return 0;
        }
        return -1;
    }

    if (!arg && request != TIOCFLUSH) return -1;

    switch (request) {
        case TCGETS: {
            for (size_t i = 0; i < sizeof(struct termios); i++) {
                ((char *)arg)[i] = ((char *)&tty->term)[i];
            }
            return 0;
        }
        case TCSETS:
        case TCSETSW:
        case TCSETSF: {
            if (request == TCSETSF) {
                input_head[tty_idx] = 0;
                input_tail[tty_idx] = 0;
            }
            for (size_t i = 0; i < sizeof(struct termios); i++) {
                ((char *)&tty->term)[i] = ((char *)arg)[i];
            }
            return 0;
        }
        case TIOCGWINSZ: {
            struct winsize *ws = (struct winsize *)arg;
            ws->ws_row = tty->rows;
            ws->ws_col = tty->cols;
            ws->ws_xpixel = global_fb.width;
            ws->ws_ypixel = global_fb.height;
            return 0;
        }
        case TIOCSWINSZ: {
            struct winsize *ws = (struct winsize *)arg;
            tty_resize(tty_idx, ws->ws_col, ws->ws_row);
            return 0;
        }
        case TIOCFLUSH: {
            int queue = (arg) ? *(int *)arg : 0;
            if (queue == 0 || queue == 2) {
                input_head[tty_idx] = 0;
                input_tail[tty_idx] = 0;
            }
            return 0;
        }
        case TIOCGPGRP: {
            *(int32_t *)arg = tty_pgrps[tty_idx];
            return 0;
        }
        case TIOCSPGRP: {
            tty_pgrps[tty_idx] = *(int32_t *)arg;
            return 0;
        }
        case TIOCGFLAGS: {
            struct tty_flags *tf = (struct tty_flags *)arg;
            tf->flags = 0;
            if (tty->cursor_enabled) tf->flags |= TTY_CURSOR_ON;
            if (tty->cursor_blink_active) tf->flags |= TTY_CURSOR_BLINK;
            tf->blink_rate = tty->blink_rate;
            return 0;
        }
        case TIOCSFLAGS: {
            struct tty_flags *tf = (struct tty_flags *)arg;

            if (tty_idx == active_tty_idx && tty->cursor_draw_state) {
                tty_draw_cursor(tty_idx, false);
            }

            tty->cursor_enabled = (tf->flags & TTY_CURSOR_ON) ? true : false;
            tty->cursor_blink_active = (tf->flags & TTY_CURSOR_BLINK) ? true : false;

            if (tf->blink_rate > 0) {
                tty->blink_rate = tf->blink_rate;
            }

            tty->cursor_tick = 0;
            tty->cursor_draw_state = tty->cursor_enabled;

            if (tty_idx == active_tty_idx && tty->cursor_draw_state) {
                tty_draw_cursor(tty_idx, true);
            }
            tty_flush_dirty();
            return 0;
        }
        default:
            return -1;
    }
}

void tty_init(framebuffer_t *fb) {
    if (!fb) return;

    global_fb = *fb;
    fb_initialized = 1;

    for (size_t i = 0; i < TTY7_MAX_FB_WIDTH * TTY7_MAX_FB_HEIGHT; i++) {
        tty7_backing_store[i] = 0;
    }

    psf1_header_t *font = (psf1_header_t *)Lat2_Terminus16_psf;
    font_height = font->charsize;

    uint32_t computed_cols = global_fb.width / font_width;
    uint32_t computed_rows = global_fb.height / font_height;

    uint32_t max_cols = (computed_cols < TTY_MAX_COLS) ? computed_cols : TTY_MAX_COLS;
    uint32_t max_rows = (computed_rows < TTY_MAX_ROWS) ? computed_rows : TTY_MAX_ROWS;

    for (uint32_t t = 0; t < 8; t++) {
        ttys[t].cols = max_cols;
        ttys[t].rows = max_rows;
        ttys[t].cursor_x = 0;
        ttys[t].cursor_y = 0;
        ttys[t].saved_x = 0;
        ttys[t].saved_y = 0;
        ttys[t].scroll_top = 0;
        ttys[t].scroll_bottom = max_rows - 1;
        ttys[t].default_fg = 0xffffffff;
        ttys[t].default_bg = 0x00000000;
        ttys[t].current_fg = 0xffffffff;
        ttys[t].current_bg = 0x00000000;
        ttys[t].term.c_lflag = ICANON;

        ttys[t].ansi_state = ANSI_STATE_NORMAL;
        ttys[t].ansi_param_count = 0;
        ttys[t].ansi_has_param = false;
        ttys[t].bold = false;
        ttys[t].underline = false;

        ttys[t].cursor_enabled = (t == 7) ? false : true;
        ttys[t].cursor_blink_active = (t == 7) ? false : true;
        ttys[t].cursor_draw_state = (t == 7) ? false : true;
        ttys[t].cursor_tick = 0;
        ttys[t].blink_rate = 100;

        ttys[t].grid = static_grid_pool[t];

        for (uint32_t r = 0; r < max_rows; r++) {
            for (uint32_t c = 0; c < max_cols; c++) {
                uint32_t offset = r * max_cols + c;
                ttys[t].grid[offset].ch = ' ';
                ttys[t].grid[offset].fg = ttys[t].default_fg;
                ttys[t].grid[offset].bg = ttys[t].default_bg;
            }
        }
    }

    for (size_t i = 0; i < MAX_PTYS; i++) {
        pty_pairs[i].allocated = false;
        pty_pairs[i].locked = false;
        pty_pairs[i].controller_to_replica.head = 0;
        pty_pairs[i].controller_to_replica.tail = 0;
        pty_pairs[i].replica_to_controller.head = 0;
        pty_pairs[i].replica_to_controller.tail = 0;
        pty_pairs[i].pgrp = 0;
    }

    active_tty_idx = 0;
    tty_redraw_active();
}

void tty_dev_init(void) {
    uint8_t tty_bitmask = DEVFS_READ | DEVFS_WRITE | DEVFS_IOCTL;
    ahci_device_t dummy_dev = {0};

    for (int i = 0; i < 8; i++) {
        char dev_name[16];
        dev_name[0] = '/'; dev_name[1] = 't'; dev_name[2] = 't'; dev_name[3] = 'y';
        dev_name[4] = '0' + i; dev_name[5] = '\0';

        register_device(
            (read_func_t)tty_read,
            (ioctl_func_t)tty_ioctl,
            (write_func_t)tty_write_dev,
            tty_bitmask, KBD, dev_name, dummy_dev, i
        );
    }

    register_device(
        (read_func_t)tty_read,
        (ioctl_func_t)tty_ioctl,
        (write_func_t)tty_write_dev,
        tty_bitmask, KBD, "/dev/ptmx", dummy_dev, 99
    );

    for (int i = 0; i < MAX_PTYS; i++) {
        char pts_name[32] = "/dev/pts/";
        if (i < 10) {
            pts_name[9] = '0' + i;
            pts_name[10] = '\0';
        } else {
            pts_name[9] = '0' + (i / 10);
            pts_name[10] = '0' + (i % 10);
            pts_name[11] = '\0';
        }

        register_device(
            (read_func_t)tty_read,
            (ioctl_func_t)tty_ioctl,
            (write_func_t)tty_write_dev,
            tty_bitmask, KBD, pts_name, dummy_dev, 200 + i
        );
    }
}

void tty_update_cursor(void) {
    if (!fb_initialized || active_tty_idx == 7) return;

    tty_t *active_tty = &ttys[active_tty_idx];
    if (!active_tty->cursor_enabled || !active_tty->cursor_blink_active) {
        return;
    }

    active_tty->cursor_tick++;
    if (active_tty->cursor_tick >= active_tty->blink_rate) {
        active_tty->cursor_tick = 0;
        active_tty->cursor_draw_state = !active_tty->cursor_draw_state;
        tty_draw_cursor(active_tty_idx, active_tty->cursor_draw_state);
        tty_flush_dirty();
    }
}

void echo_off() { if (active_tty_idx != 7) { ttys[active_tty_idx].term.c_lflag &= ~ECHO; echo = 0; } }
void echo_on() { if (active_tty_idx != 7) { ttys[active_tty_idx].term.c_lflag |= ECHO; echo = 1; } }
int echo_is_on() { return (active_tty_idx == 7) ? 0 : ((ttys[active_tty_idx].term.c_lflag & ECHO) && echo); }

static void tty_clear_tty(uint32_t tty_idx) {
    if (tty_idx >= 8) return;

    if (tty_idx == 7) {
        for (size_t i = 0; i < TTY7_MAX_FB_WIDTH * TTY7_MAX_FB_HEIGHT; i++) {
            tty7_backing_store[i] = 0;
        }
        if (active_tty_idx == 7) {
            tty_draw_rect(0, 0, global_fb.width, global_fb.height, 0xFF000000);
            tty_flush_dirty();
        }
        return;
    }

    tty_t *tty = &ttys[tty_idx];
    if (!tty->grid) return;

    // Reset Cursor Positions to (0,0)
    tty->cursor_x = 0;
    tty->cursor_y = 0;

    uint32_t total_cells = tty->rows * tty->cols;
    for (uint32_t i = 0; i < total_cells; i++) {
        tty->grid[i].ch = ' ';
        tty->grid[i].fg = tty->current_fg;
        tty->grid[i].bg = tty->current_bg;
    }

    // Fully repaint background rectangle & active grid
    if (tty_idx == active_tty_idx) {
        tty_draw_rect(0, 0, global_fb.width, global_fb.height, tty->current_bg);
        tty_redraw_active();
    }
}

void tty_clear(void) {
    tty_clear_tty(active_tty_idx);
}