// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
size_t strlen(const char *s);
int strcmp(const char *s1, const char *s2);
char *strcat(char *destination, const char *source);
char *strcpy(char *destination, const char *source);
char *strncpy(char *destination, const char *source, size_t n);
char *strchr(const char *str, int c);
char *strrchr(const char *str, int c);
