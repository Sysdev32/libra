// user.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
extern int start_micropython_task(const char* code);

#define CHUNK_SIZE 512

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    // 1. Open file in binary read mode
    FILE* f = fopen("main.py", "rb");
    if (f == NULL) {
        perror("Error opening main.py");
        return 1;
    }

    char* code_buffer = NULL;
    size_t total_size = 0;
    char chunk[CHUNK_SIZE];
    size_t bytes_read;
    // 2. Read the file sequentially block by block
    while ((bytes_read = fread(chunk, 1, CHUNK_SIZE, f)) > 0) {
        // Dynamically grow the code buffer to fit the new chunk
        char* new_buffer = realloc(code_buffer, total_size + bytes_read + 1);
        if (new_buffer == NULL) {
            fprintf(stderr, "Error: System ran out of memory while reading file.\n");
            free(code_buffer);
            fclose(f);
            return 1;
        }
        code_buffer = new_buffer;

        // Append the new chunk to our master buffer
        memcpy(code_buffer + total_size, chunk, bytes_read);
        total_size += bytes_read;
    }

    fclose(f);

    // 3. Ensure the final script is safely null-terminated
    if (code_buffer != NULL) {
        code_buffer[total_size] = '\0';
        
        printf("--- Executing main.py Content (%zu bytes) ---\n", total_size);
        
        // 4. Boot MicroPython safely
        start_micropython_task(code_buffer);
        
        // 5. Cleanup
        free(code_buffer);
    } else {
        printf("Warning: main.py is empty.\n");
        start_micropython_task(""); // Run empty string to safely pass initialization
    }
    printf("executing protection fault test now!\n");
    asm volatile ("hlt");
    return 0;
}
