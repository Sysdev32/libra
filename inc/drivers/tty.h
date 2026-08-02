#pragma once
#define NUM_TTYS 6
#include <stdint.h>
#include <stdbool.h>

#define TCGETS          0x5401
#define TCSETS          0x5402
#define TIOCGWINSZ      0x5413

#define ECHO            0x0008
#define ICANON          0x0002

// Dynamic Cursor Control IOCTLs
#define TIOCGFLAGS      0x5470  // Get cursor configuration/flags
#define TIOCSFLAGS      0x5471  // Set cursor configuration/flags

// Flag bitmasks for cursor controls
#define TTY_CURSOR_ON       0x01  // Is the cursor enabled?
#define TTY_CURSOR_BLINK    0x02  // Does the cursor blink, or is it solid?

#define ANSI_MAX_PARAMS 16

typedef uint32_t tcflag_t;
typedef unsigned char cc_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[19];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

// IOCTL struct payload for retrieving/setting cursor configurations
struct tty_flags {
    uint32_t flags;       // Combination of TTY_CURSOR_ON and TTY_CURSOR_BLINK
    uint32_t blink_rate;  // Blink period in timer ticks
};

typedef struct {
    char ch;
    uint32_t fg;
    uint32_t bg;
} __attribute__((packed)) tty_cell_t;

typedef enum {
    ANSI_STATE_NORMAL,
    ANSI_STATE_ESC,
    ANSI_STATE_CSI
} ansi_state_t;

typedef struct {
    tty_cell_t *grid;      
    uint32_t cols;         
    uint32_t rows;         
    
    uint32_t cursor_x;
    uint32_t cursor_y;
    
    // Save/Restore Cursor registers
    uint32_t saved_x;
    uint32_t saved_y;

    // Scrolling margins (0-indexed, inclusive)
    uint32_t scroll_top;
    uint32_t scroll_bottom;
    
    uint32_t default_fg;
    uint32_t default_bg;
    uint32_t current_fg;
    uint32_t current_bg;

    struct termios term;

    ansi_state_t ansi_state;
    uint32_t ansi_params[ANSI_MAX_PARAMS];
    uint32_t ansi_param_count;
    bool ansi_has_param;
    bool bold;
    bool underline;

    // Controllable Blinking Cursor State
    bool cursor_enabled;      // Controlled via TIOCSFLAGS (TTY_CURSOR_ON)
    bool cursor_blink_active; // Controlled via TIOCSFLAGS (TTY_CURSOR_BLINK)
    bool cursor_draw_state;   // Is the block currently inverted on screen?
    uint32_t cursor_tick;     // Incremental ticker
    uint32_t blink_rate;      // Threshold for switching states
} tty_t;

typedef struct {
    uint32_t *address;
    uint32_t width;
    uint32_t height;
    uint32_t pitch; 
} framebuffer_t;

// Periodic system timer hook
void tty_update_cursor(void);
void tty_switch(uint32_t tty_idx);