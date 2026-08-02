#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <fs/mnt.h>
#include "font.h"
#include <drivers/tty.h>
#define BIT(x) (1ULL << (x))

// Standard terminal IOCTL definitions
#define TCGETS          0x5401
#define TCSETS          0x5402
#define TCSETSW         0x5403  // Set termios after draining output
#define TCSETSF         0x5404  // Set termios after drain and flush input
#define TIOCGWINSZ      0x5413  // Get window size
#define TIOCSWINSZ      0x5414  // Set window size (RESIZE)
#define TIOCFLUSH       0x540B  // Flush queues
#define TIOCGPGRP       0x540F  // Get foreground process group
#define TIOCSPGRP       0x5410  // Set foreground process group
#define TIOCSPTLCK      0x40045431 // Lock/Unlock replica PTY
#define TIOCGPTN        0x80045430 // Get PTY pair number

#define TTY_MAX_COLS    256
#define TTY_MAX_ROWS    128
#define MAX_PTYS        32         // Maximum number of concurrent dynamic PTY pairs

#define PSF1_MAGIC0     0x36
#define PSF1_MAGIC1     0x04
#define PSF1_MODE512    0x01

extern char kgetc(void);
int echo = 1;

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
static tty_t ttys[8]; // Fixed mapping for physical VTs tty0 through tty7
static uint32_t active_tty_idx = 1;
static uint32_t font_height = 16; 
static const uint32_t font_width = 8; 

// Raw byte tracking stream pointers for tty7 raw pixel mapping
static size_t tty7_write_offset = 0;
static size_t tty7_read_offset = 0;

// Input/Output Ring Buffers for termios flushing emulation
#define TTY_BUF_SIZE 1024
static char input_buffers[8][TTY_BUF_SIZE];
static uint32_t input_head[8] = {0};
static uint32_t input_tail[8] = {0};

static tty_cell_t static_grid_pool[8][TTY_MAX_ROWS * TTY_MAX_COLS];

// Foreground process group IDs for job control support
static int32_t tty_pgrps[8] = {0};

// --- Linux Unix98 PTY Internal Structures ---
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

// --- PTY Internal Ring Buffer Mechanics ---
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

// --- Core Renderer ---
void tty_draw_char_psf1(framebuffer_t *fb, char c, int cx, int cy, uint32_t fg_color, uint32_t bg_color) {
    if (!fb || !fb->address) return;
    psf1_header_t *font = (psf1_header_t *)Lat2_Terminus16_psf;
    if (font->magic[0] != PSF1_MAGIC0 || font->magic[1] != PSF1_MAGIC1) return;

    int num_glyphs = (font->mode & PSF1_MODE512) ? 512 : 256;
    unsigned char *glyph = (unsigned char *)Lat2_Terminus16_psf + 
                           sizeof(psf1_header_t) + 
                           ((unsigned char)c < num_glyphs ? (unsigned char)c : 0) * font->charsize;

    for (uint32_t y = 0; y < font->charsize; y++) {
        unsigned char row_byte = glyph[y];
        for (uint32_t x = 0; x < font_width; x++) {
            uint32_t bit_mask = 0x80 >> x;
            uint32_t color = (row_byte & bit_mask) ? fg_color : bg_color;
            uint32_t screen_x = cx + x;
            uint32_t screen_y = cy + y;

            if (screen_x < fb->width && screen_y < fb->height) {
                fb->address[screen_y * (fb->pitch / 4) + screen_x] = color;
            }
        }
    }
}

// --- Cursor Painting Engine ---
static void tty_draw_cursor(uint32_t tty_idx, bool show) {
    if (tty_idx == 7) return; 
    if (!fb_initialized || tty_idx != active_tty_idx) return;
    tty_t *tty = &ttys[tty_idx];
    if (!tty->grid || !tty->cursor_enabled) return;

    uint32_t col = tty->cursor_x;
    uint32_t row = tty->cursor_y;

    if (col >= tty->cols || row >= tty->rows) return;

    uint32_t offset = row * tty->cols + col;
    tty_cell_t cell = tty->grid[offset];

    if (show) {
        tty_draw_char_psf1(&global_fb, cell.ch, col * font_width, row * font_height, cell.bg, cell.fg);
    } else {
        tty_draw_char_psf1(&global_fb, cell.ch, col * font_width, row * font_height, cell.fg, cell.bg);
    }
}

// --- Screen Management & Resizing Utilities ---
static void tty_flush_cell(uint32_t tty_idx, uint32_t col, uint32_t row) {
    if (tty_idx == 7) return; 
    if (!fb_initialized || tty_idx != active_tty_idx) return;
    tty_t *tty = &ttys[tty_idx];
    if (!tty->grid || col >= tty->cols || row >= tty->rows) return;

    tty_cell_t cell = tty->grid[row * tty->cols + col];
    tty_draw_char_psf1(&global_fb, cell.ch, col * font_width, row * font_height, cell.fg, cell.bg);
}

static void tty_redraw_active() {
    if (!fb_initialized) return;
    if (active_tty_idx == 7) return; 

    tty_t *tty = &ttys[active_tty_idx];
    if (!tty->grid) return;

    uint32_t active_pixels_x = tty->cols * font_width;
    uint32_t active_pixels_y = tty->rows * font_height;
    
    for (uint32_t y = 0; y < global_fb.height; y++) {
        for (uint32_t x = 0; x < global_fb.width; x++) {
            if (x >= active_pixels_x || y >= active_pixels_y) {
                global_fb.address[y * (global_fb.pitch / 4) + x] = tty->default_bg;
            }
        }
    }

    for (uint32_t r = 0; r < tty->rows; r++) {
        for (uint32_t c = 0; c < tty->cols; c++) {
            tty_flush_cell(active_tty_idx, c, r);
        }
    }

    if (tty->cursor_enabled && tty->cursor_draw_state) {
        tty_draw_cursor(active_tty_idx, true);
    }
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

    for (uint32_t r = top; r < bottom; r++) {
        memcpy(&tty->grid[r * tty->cols], 
               &tty->grid[(r + 1) * tty->cols], 
               sizeof(tty_cell_t) * tty->cols);
    }

    uint32_t last_row_offset = bottom * tty->cols;
    for (uint32_t c = 0; c < tty->cols; c++) {
        tty->grid[last_row_offset + c].ch = ' ';
        tty->grid[last_row_offset + c].fg = tty->current_fg;
        tty->grid[last_row_offset + c].bg = tty->current_bg;
    }

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
    memcpy(temp_grid, tty->grid, sizeof(tty_cell_t) * tty->rows * tty->cols);

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
    tty_redraw_active();
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

    if (c == '\n') {
        tty->cursor_x = 0;
        tty->cursor_y++;
    } 
    else if (c == '\r') {
        tty->cursor_x = 0;
    } 
    else if (c == '\b') {
        if (tty->cursor_x > 0) tty->cursor_x--;
        else if (tty->cursor_y > tty->scroll_top) {
            tty->cursor_y--;
            tty->cursor_x = tty->cols - 1;
        } else goto draw_cursor_after_write;

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
        if (tty->cursor_x < tty->cols && tty->cursor_y <= tty->scroll_bottom) {
            uint32_t offset = tty->cursor_y * tty->cols + tty->cursor_x;
            tty->grid[offset].ch = c;
            tty->grid[offset].fg = tty->current_fg;
            tty->grid[offset].bg = tty->current_bg;
            tty_flush_cell(tty_idx, tty->cursor_x, tty->cursor_y);
            tty->cursor_x++;
        }
    }

    if (tty->cursor_x >= tty->cols) {
        tty->cursor_x = 0;
        tty->cursor_y++;
    }

    if (tty->cursor_y > tty->scroll_bottom) {
        tty->cursor_y = tty->scroll_bottom;
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
}

void tty_putchar(char c) {
    tty_putchar_to(active_tty_idx, c);
}

void tty_write(const char *str) {
    while (*str) tty_putchar(*str++);
}

void tty_write_to(uint32_t tty_idx, const char *str) {
    while (*str) tty_putchar_to(tty_idx, *str++);
}

// --- Dynamic Keyboard Input Routing API ---
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

// --- Driver VFS Read Callbacks ---
size_t tty_read(int fd, void *buf, size_t count, int offset) {
    if (!buf || count == 0) return 0;

    // --- Dynamic Linux/Unix98 PTY Parsing Logic ---
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

    // --- tty7: Pure raw pixel memory mapping interface ---
    if (active_tty_idx == 7) {
        if (!fb_initialized || !global_fb.address) return 0;
        
        size_t total_fb_bytes = global_fb.height * global_fb.pitch;
        size_t read_ptr = (offset > 0) ? (size_t)offset : tty7_read_offset;
        
        if (read_ptr >= total_fb_bytes) {
            if (offset <= 0) tty7_read_offset = 0;
            return 0; 
        }

        if (read_ptr + count > total_fb_bytes) {
            count = total_fb_bytes - read_ptr;
        }

        uint8_t *fb_byte_ptr = (uint8_t *)global_fb.address;
        memcpy(buf, fb_byte_ptr + read_ptr, count);
        
        if (offset <= 0) {
            tty7_read_offset = (read_ptr + count) % total_fb_bytes;
        }
        return count;
    }

    // --- Standard VTs (tty0 - tty6) cooked/raw processing loop ---
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
        if (!fb_initialized || !global_fb.address) return 0;

        size_t total_fb_bytes = global_fb.height * global_fb.pitch;
        uint8_t *fb_byte_ptr = (uint8_t *)global_fb.address;
        const uint8_t *src_pixels = (const uint8_t *)buf;

        for (size_t i = 0; i < count; i++) {
            fb_byte_ptr[tty7_write_offset] = src_pixels[i];
            tty7_write_offset++;
            if (tty7_write_offset >= total_fb_bytes) {
                tty7_write_offset = 0; 
            }
        }
        return count;
    }

    const char *char_buf = (const char *)buf;
    for (size_t i = 0; i < count; i++) {
        tty_putchar_to(active_tty_idx, char_buf[i]);
    }
    return count;
}

// --- MASTER IOCTL HANDLER ---
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
            case TCGETS: memcpy((struct termios *)arg, &pair->term, sizeof(struct termios)); return 0;
            case TCSETS:
            case TCSETSW:
            case TCSETSF: memcpy(&pair->term, (struct termios *)arg, sizeof(struct termios)); return 0;
            case TIOCGWINSZ: memcpy((struct winsize *)arg, &pair->winsz, sizeof(struct winsize)); return 0;
            case TIOCSWINSZ: memcpy(&pair->winsz, (struct winsize *)arg, sizeof(struct winsize)); return 0;
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
            memcpy((struct termios *)arg, &tty->term, sizeof(struct termios));
            return 0;
        }
        case TCSETS: 
        case TCSETSW:
        case TCSETSF: {
            if (request == TCSETSF) {
                input_head[tty_idx] = 0;
                input_tail[tty_idx] = 0; 
            }
            memcpy(&tty->term, (struct termios *)arg, sizeof(struct termios));
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
            return 0;
        }
        default:
            return -1; 
    }
}

// --- Initialization & DevFS Device Registration ---
void tty_init(framebuffer_t *fb) {
    if (!fb) return;

    global_fb = *fb; 
    fb_initialized = 1;

    psf1_header_t *font = (psf1_header_t *)Lat2_Terminus16_psf;
    font_height = font->charsize;

    uint32_t computed_cols = global_fb.width / font_width;
    uint32_t computed_rows = global_fb.height / font_height;

    uint32_t max_cols = (computed_cols < TTY_MAX_COLS) ? computed_cols : TTY_MAX_COLS;
    uint32_t max_rows = (computed_rows < TTY_MAX_ROWS) ? computed_rows : TTY_MAX_ROWS;

    uint32_t colors[8] = {
        0xFFFFFFFF, 0xFF00FF00, 0xFF00FFFF, 0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF, 0x00000000
    };

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

    memset(pty_pairs, 0, sizeof(pty_pairs));
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
    }
}

void echo_off() { if (active_tty_idx != 7) { ttys[active_tty_idx].term.c_lflag &= ~ECHO; echo = 0; } }
void echo_on() { if (active_tty_idx != 7) { ttys[active_tty_idx].term.c_lflag |= ECHO; echo = 1; } }
int echo_is_on() { return (active_tty_idx == 7) ? 0 : ((ttys[active_tty_idx].term.c_lflag & ECHO) && echo); }
static void tty_clear_tty(uint32_t tty_idx) {
    if (tty_idx >= 8 || tty_idx == 7) return;
    tty_t *tty = &ttys[tty_idx];
    if (!tty->grid) return;

    // Reset grid cells to spaces using current terminal colors
    uint32_t total_cells = tty->rows * tty->cols;
    for (uint32_t i = 0; i < total_cells; i++) {
        tty->grid[i].ch = ' ';
        tty->grid[i].fg = tty->current_fg;
        tty->grid[i].bg = tty->current_bg;
    }

    // Reset cursor to top-left corner
    tty->cursor_x = 0;
    tty->cursor_y = 0;

    // Redraw screen if clearing the active TTY
    if (tty_idx == active_tty_idx) {
        tty_redraw_active();
    }
}

void tty_clear(void) {
    tty_clear_tty(active_tty_idx);
}
/**
 * @brief Draws a single pixel directly to the active framebuffer.
 *        Strictly restricted to run only when VT tty7 is active.
 *
 * @param x Pixel X coordinate (0 to width - 1)
 * @param y Pixel Y coordinate (0 to height - 1)
 * @param color 32-bit ARGB/XRGB color (e.g., 0xFFRRGGBB)
 */
void tty_draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    // 1. Enforce VT lock: strictly exit if active TTY is not tty7
    if (active_tty_idx != 7) {
        return;
    }

    // 2. Safeguard against missing/uninitialized framebuffer
    if (!fb_initialized || !global_fb.address) {
        return;
    }

    // 3. Boundary check to prevent kernel memory corruption
    if (x >= global_fb.width || y >= global_fb.height) {
        return;
    }

    // 4. Calculate pixel position using pitch (pitch is in bytes, global_fb.address is uint32_t*)
    uint32_t stride = global_fb.pitch / sizeof(uint32_t);
    global_fb.address[y * stride + x] = color;
}