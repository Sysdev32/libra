#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>

#include <zlib.h>
#include <png.h>

extern int start_micropython_task(const char *code);
extern void draw_rect(int x, int y, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b);

#define CHUNK_SIZE 512
#define IMG_WIDTH  150
#define IMG_HEIGHT 150

struct mem_writer {
    unsigned char *data;
    size_t size;
    size_t capacity;
};

struct mem_reader {
    const unsigned char *data;
    size_t size;
    size_t offset;
};

static struct mem_writer png_output = {0};

static void png_mem_write(
    png_structp png_ptr,
    png_bytep data,
    png_size_t length)
{
    struct mem_writer *mem =
        (struct mem_writer *)png_get_io_ptr(png_ptr);

    if (mem->size + length > mem->capacity) {
        size_t new_capacity =
            mem->capacity ? mem->capacity * 2 : 1024;

        while (new_capacity < mem->size + length)
            new_capacity *= 2;

        unsigned char *new_data =
            realloc(mem->data, new_capacity);

        if (!new_data)
            png_error(png_ptr, "Out of memory");

        mem->data = new_data;
        mem->capacity = new_capacity;
    }

    memcpy(mem->data + mem->size, data, length);
    mem->size += length;
}

static void png_mem_flush(png_structp png_ptr)
{
    (void)png_ptr;
}

static void png_mem_read(
    png_structp png_ptr,
    png_bytep out,
    png_size_t length)
{
    struct mem_reader *mem =
        (struct mem_reader *)png_get_io_ptr(png_ptr);

    if (mem->offset + length > mem->size)
        png_error(png_ptr, "Unexpected end of PNG");

    memcpy(out, mem->data + mem->offset, length);
    mem->offset += length;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ------------------------------------------------------------ */
    /* Permission test                                               */
    /* ------------------------------------------------------------ */

    int elf_fd = open("user_app.elf", O_RDONLY);

    if (elf_fd < 0) {
        perror("Error opening user_app.elf");
    } else {
        struct stat st;

        if (fstat(elf_fd, &st) < 0) {
            perror("fstat failed");
        } else {
            uint32_t permissions = st.st_mode & 00777;

            uint32_t u = (permissions >> 6) & 7;
            uint32_t g = (permissions >> 3) & 7;
            uint32_t o = permissions & 7;

            printf("--- user_app.elf Permissions Info ---\n");
            printf("Raw Octal: 0%o\n", permissions);

            printf(
                "Owner (u): %c%c%c\n",
                (u & 4) ? 'r' : '-',
                (u & 2) ? 'w' : '-',
                (u & 1) ? 'x' : '-');

            printf(
                "Group (g): %c%c%c\n",
                (g & 4) ? 'r' : '-',
                (g & 2) ? 'w' : '-',
                (g & 1) ? 'x' : '-');

            printf(
                "Other (o): %c%c%c\n",
                (o & 4) ? 'r' : '-',
                (o & 2) ? 'w' : '-',
                (o & 1) ? 'x' : '-');

            printf("--------------------------------------\n");
        }

        close(elf_fd);
    }

    /* ------------------------------------------------------------ */
    /* Read main.py                                                  */
    /* ------------------------------------------------------------ */

    FILE *f = fopen("main.py", "rb");

    if (!f) {
        perror("Error opening main.py");
        return 1;
    }

    char *code_buffer = NULL;
    size_t total_size = 0;

    char chunk[CHUNK_SIZE];
    size_t bytes_read;

    while ((bytes_read = fread(chunk, 1, CHUNK_SIZE, f)) > 0) {
        char *new_buffer =
            realloc(code_buffer,
                    total_size + bytes_read + 1);

        if (!new_buffer) {
            fprintf(stderr, "Out of memory while reading main.py\n");
            free(code_buffer);
            fclose(f);
            return 1;
        }

        code_buffer = new_buffer;
        memcpy(code_buffer + total_size, chunk, bytes_read);
        total_size += bytes_read;
    }

    fclose(f);

    if (code_buffer) {
        code_buffer[total_size] = '\0';
        printf("--- Executing main.py (%zu bytes) ---\n", total_size);
        start_micropython_task(code_buffer);
        free(code_buffer);
    } else {
        printf("main.py is empty.\n");
        start_micropython_task("");
    }

    /* ------------------------------------------------------------ */
    /* zlib test                                                     */
    /* ------------------------------------------------------------ */

    const char *text = "Hello from Libra!";
    uLong src_len = strlen(text) + 1;

    Bytef compressed[128];
    Bytef decompressed[128];

    uLong comp_len = sizeof(compressed);
    uLong decomp_len = sizeof(decompressed);

    if (compress(compressed, &comp_len, (const Bytef *)text, src_len) != Z_OK) {
        printf("compress failed\n");
        return 1;
    }

    if (uncompress(decompressed, &decomp_len, compressed, comp_len) != Z_OK) {
        printf("uncompress failed\n");
        return 1;
    }

    printf("Original: %s\n", text);
    printf("Compressed size: %lu\n", (unsigned long)comp_len);
    printf("Decompressed: %s\n", (char *)decompressed);

    /* ------------------------------------------------------------ */
    /* libpng test: Encode Gradient to Memory                       */
    /* ------------------------------------------------------------ */

    printf("--- libpng: Encoding %dx%d Gradient ---\n", IMG_WIDTH, IMG_HEIGHT);

    png_structp write_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!write_ptr) {
        fprintf(stderr, "Failed to create png write struct\n");
        return 1;
    }

    png_infop write_info_ptr = png_create_info_struct(write_ptr);
    if (!write_info_ptr) {
        png_destroy_write_struct(&write_ptr, NULL);
        fprintf(stderr, "Failed to create png write info struct\n");
        return 1;
    }

    if (setjmp(png_jmpbuf(write_ptr))) {
        png_destroy_write_struct(&write_ptr, &write_info_ptr);
        free(png_output.data);
        memset(&png_output, 0, sizeof(png_output));
        fprintf(stderr, "Error during png encoding\n");
        return 1;
    }

    png_set_write_fn(write_ptr, &png_output, png_mem_write, png_mem_flush);

    png_set_IHDR(
        write_ptr,
        write_info_ptr,
        IMG_WIDTH,
        IMG_HEIGHT,
        8,
        PNG_COLOR_TYPE_RGB,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT
    );

    png_write_info(write_ptr, write_info_ptr);

    // Dynamic heap allocation for the row pointers to preserve tiny thread stacks
    png_bytep *write_rows = malloc(sizeof(png_bytep) * IMG_HEIGHT);
    if (!write_rows) {
        fprintf(stderr, "Out of memory allocating write rows index array\n");
        png_destroy_write_struct(&write_ptr, &write_info_ptr);
        free(png_output.data);
        return 1;
    }
    memset(write_rows, 0, sizeof(png_bytep) * IMG_HEIGHT);

    size_t row_bytes = png_get_rowbytes(write_ptr, write_info_ptr);
    for (int y = 0; y < IMG_HEIGHT; y++) {
        write_rows[y] = malloc(row_bytes);
        if (!write_rows[y]) {
            fprintf(stderr, "Out of memory allocating write row buffer %d\n", y);
            for (int i = 0; i < y; i++) free(write_rows[i]);
            free(write_rows);
            png_destroy_write_struct(&write_ptr, &write_info_ptr);
            free(png_output.data);
            return 1;
        }

        for (int x = 0; x < IMG_WIDTH; x++) {
            write_rows[y][x * 3 + 0] = (uint8_t)((x * 255) / IMG_WIDTH);  // Red
            write_rows[y][x * 3 + 1] = (uint8_t)((y * 255) / IMG_HEIGHT); // Green
            write_rows[y][x * 3 + 2] = 128;                               // Blue
        }
    }

    png_write_image(write_ptr, write_rows);
    png_write_end(write_ptr, NULL);

    // Clean up write assets
    for (int y = 0; y < IMG_HEIGHT; y++) {
        free(write_rows[y]);
    }
    free(write_rows);
    png_destroy_write_struct(&write_ptr, &write_info_ptr);

    printf("Successfully encoded gradient into memory buffer (%zu bytes)\n", png_output.size);

    /* ------------------------------------------------------------ */
    /* libpng test: Decode and Draw Using draw_rect                */
    /* ------------------------------------------------------------ */

    printf("--- libpng: Decoding and Rendering Buffer ---\n");

    struct mem_reader png_input = {
        .data = png_output.data,
        .size = png_output.size,
        .offset = 0
    };

    png_structp read_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!read_ptr) {
        fprintf(stderr, "Failed to create png read struct\n");
        free(png_output.data);
        return 1;
    }

    png_infop read_info_ptr = png_create_info_struct(read_ptr);
    if (!read_info_ptr) {
        png_destroy_read_struct(&read_ptr, NULL, NULL);
        free(png_output.data);
        return 1;
    }

    if (setjmp(png_jmpbuf(read_ptr))) {
        png_destroy_read_struct(&read_ptr, &read_info_ptr, NULL);
        free(png_output.data);
        fprintf(stderr, "Error during png decoding\n");
        return 1;
    }

    png_set_read_fn(read_ptr, &png_input, png_mem_read);
    png_read_info(read_ptr, read_info_ptr);

    png_uint_32 width = png_get_image_width(read_ptr, read_info_ptr);
    png_uint_32 height = png_get_image_height(read_ptr, read_info_ptr);
    png_byte color_type = png_get_color_type(read_ptr, read_info_ptr);
    png_byte bit_depth = png_get_bit_depth(read_ptr, read_info_ptr);

    if (bit_depth == 16) png_set_strip_16(read_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(read_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(read_ptr);
    if (png_get_valid(read_ptr, read_info_ptr, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(read_ptr);
    if (color_type == PNG_COLOR_TYPE_RGB_ALPHA || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_strip_alpha(read_ptr);

    png_read_update_info(read_ptr, read_info_ptr);

    // Safe heap allocation for decoding pointers
    png_bytep *read_rows = malloc(sizeof(png_bytep) * height);
    if (!read_rows) {
        fprintf(stderr, "Out of memory allocating read rows index array\n");
        png_destroy_read_struct(&read_ptr, &read_info_ptr, NULL);
        free(png_output.data);
        return 1;
    }
    memset(read_rows, 0, sizeof(png_bytep) * height);

    size_t read_row_bytes = png_get_rowbytes(read_ptr, read_info_ptr);
    for (png_uint_32 y = 0; y < height; y++) {
        read_rows[y] = malloc(read_row_bytes);
        if (!read_rows[y]) {
            fprintf(stderr, "Out of memory allocating read row buffer %u\n", y);
            for (png_uint_32 i = 0; i < y; i++) free(read_rows[i]);
            free(read_rows);
            png_destroy_read_struct(&read_ptr, &read_info_ptr, NULL);
            free(png_output.data);
            return 1;
        }
    }

    png_read_image(read_ptr, read_rows);
    png_read_end(read_ptr, NULL);

    // Plot pixels cleanly using the hardware layout abstraction
    for (png_uint_32 y = 0; y < height; y++) {
        for (png_uint_32 x = 0; x < width; x++) {
            uint8_t r = read_rows[y][x * 3 + 0];
            uint8_t g = read_rows[y][x * 3 + 1];
            uint8_t b = read_rows[y][x * 3 + 2];

            draw_rect((int)x, (int)y, 1, 1, r, g, b);
        }
    }
    // Final resource cleanup
    for (png_uint_32 y = 0; y < height; y++) {
        free(read_rows[y]);
    }
    free(read_rows);
    png_destroy_read_struct(&read_ptr, &read_info_ptr, NULL);
    free(png_output.data);

    return 0;
}