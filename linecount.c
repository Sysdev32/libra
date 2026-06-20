#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>

static long count_lines_in_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    long lines = 0;
    int c;

    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') lines++;
    }

    fclose(f);
    return lines;
}

static long count_lines_recursive(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;

    struct dirent *entry;
    struct stat st;
    char path[1024];

    long total = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;

        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                total += count_lines_recursive(path);
            } else if (S_ISREG(st.st_mode)) {
                total += count_lines_in_file(path);
            }
        }
    }

    closedir(dir);
    return total;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: %s <dir>\n", argv[0]);
        return 1;
    }

    long total = count_lines_recursive(argv[1]);
    printf("total lines: %ld\n", total);

    return 0;
}