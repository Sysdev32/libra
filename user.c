#include <stdio.h>
#include <stdlib.h>
#include <carrera.h>
#include <string.h>
static const char test_keymap[128] = {
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't', [0x15] = 'y',
    [0x1e] = 'a', [0x1f] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
    [0x2c] = 'z', [0x2d] = 'x', [0x2e] = 'c', [0x2f] = 'v', [0x30] = 'b', [0x39] = ' '
};
#include <stdio.h>

void read_file_example() {
    FILE *f = fopen("mytxt", "r");
    if (!f) return;

    // This gets the Newlib file descriptor (e.g., 3)
    int newlib_fd = fileno(f); 

    // This converts it back to your kernel's internal index (e.g., 1)
    int kernel_fd = newlib_fd; 

    char buffer[256];
    
    // fgets reads until a newline (\n) or until the buffer is full
    while (fgets(buffer, sizeof(buffer), f) != NULL) {
        // Print the line using your working stdout/write stub
        printf("%s\n", buffer); 
    }
    const char *text = "Hello from my custom kernel user-space!\n";
    size_t data_len = strlen(text);
    fclose(f);
}
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    int rect_x;
    int rect_y;
    int rect_width;
    int rect_height;
} packet;
int main() {
    // Force stdout to unbuffered so you see exactly when crashes/loops happen
    setvbuf(stdout, NULL, _IONBF, 0); 
    printf("hello!\n");
    read_file_example();
    packet *buf = calloc(1, sizeof(packet));
    uint32_t pid = 0;
    ipc_recv(buf, sizeof(packet), &pid);
    graduate();
    draw_rect(buf->rect_x, buf->rect_y, buf->rect_width, buf->rect_height, buf->r, buf->g, buf->b);
    return 0;
}
