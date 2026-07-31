#pragma once
#include <stdint.h>
#include <stddef.h>
int canonicalize_path(const char *input, char *out, size_t out_size);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);