#pragma once
#include <stdint.h>
#include <stddef.h>
#define KBD_BUFFER_SIZE 256
#define MOUSE_BUFFER_SIZE 128
typedef struct {
    uint8_t data[KBD_BUFFER_SIZE];
    volatile uint32_t head;  // Must be volatile
    volatile uint32_t tail;  // Must be volatile
} kbd_buffer_t;

typedef struct {
    int8_t dx;
    int8_t dy;
    uint8_t buttons;
    uint8_t reserved;
} mouse_event_t;

typedef struct {
    mouse_event_t data[MOUSE_BUFFER_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
} mouse_buffer_t;

void keyboard_init(void);
void mouse_dispatch();
int read_mouse(void *user_buffer, uint64_t count);
int mouse_buffer_pop(mouse_event_t *out_event);
void ps2_enable_mouse(void);
void kbd_buffer_push(uint8_t scancode);
int kbd_buffer_pop(uint8_t *out_scancode);
void mouse_buffer_push(mouse_event_t event);
int mouse_buffer_pop(mouse_event_t *out_event);
int read_mouse(void *user_buffer, uint64_t count);
void keyboard_dispatch();