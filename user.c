#include <stdio.h>
#include <carrera.h>
static const char test_keymap[128] = {
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't', [0x15] = 'y',
    [0x1e] = 'a', [0x1f] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
    [0x2c] = 'z', [0x2d] = 'x', [0x2e] = 'c', [0x2f] = 'v', [0x30] = 'b', [0x39] = ' '
};
int main() {
    int ch; // Always use int, not char

    printf("Enter a character: ");
    ch = getchar(); 

    printf("You entered: %c\n", ch);
    printf("hello!\n");
    graduate();
    draw_rect(0, 0, 100, 100, 255, 0, 0);
    return 0;
}