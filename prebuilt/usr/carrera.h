#ifndef LA_CARRERA_H
#define LA_CARRERA_H

#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <sys/mman.h>

typedef struct {
    int8_t dx;
    int8_t dy;
    uint8_t buttons;
    uint8_t reserved;
} mouse_event_t;

void graduate();
void draw_rect(int rect_x, int rect_y, int rect_width, int rect_height, 
                   uint8_t r, uint8_t g, uint8_t b);
int ipc_send(uint32_t target_pid, const void *buf, uint32_t size);
int ipc_recv(void *buf, uint32_t max_size, uint32_t *out_sender_pid);
int createf(void* data, const char *name, int len);
void draw_image(int start_x, int start_y, int img_w, int img_h, const uint8_t *rgb_data);
int read_mouse(void *buf, int len);
int spawn(char* path);
int waitpid(int pid);
#endif