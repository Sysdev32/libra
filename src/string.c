// SPDX-License-Identifier: GPL-3.0-only
#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }

    return dest;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    unsigned char val = (unsigned char)c;

    for (size_t i = 0; i < n; i++) {
        p[i] = val;
    }

    return s;
}
size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}
#include <string.h>
#include <stddef.h>

void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;

    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    return 0;
}
char* strcat(char* destination, const char* source) {
    // 1. Save the beginning of the destination string to return later
    char* ptr = destination;

    // 2. Move the destination pointer forward until it hits the null terminator
    while (*ptr != '\0') {
        ptr++;
    }

    // 3. Copy the characters from the source string into the destination string
    while (*source != '\0') {
        *ptr = *source;
        ptr++;
        source++;
    }

    // 4. Add the final null terminator to the end of the combined string
    *ptr = '\0';

    // Return the original destination pointer
    return destination;
}
char* strchr(const char* str, int c) {
    char target = (char)c;

    // Scan the string until we find the character or hit the null terminator
    while (*str != '\0') {
        if (*str == target) {
            return (char*)str; // Found the character, return its memory address
        }
        str++;
    }

    // Special case: If the caller is looking for the null terminator itself
    if (target == '\0') {
        return (char*)str;
    }

    // Character was not found in the string
    return NULL;
}
/**
 * Locates the last occurrence of a character in a string.
 * @param str The null-terminated string to search.
 * @param c   The character to look for (passed as an int, but evaluated as a char).
 * @return A pointer to the last matched character, or NULL if the character is not found.
 */
char* strrchr(const char* str, int c) {
    char target = (char)c;
    char* last_match = NULL;

    // Scan the entire string from left to right
    while (*str != '\0') {
        if (*str == target) {
            last_match = (char*)str; // Update the pointer every time we see a match
        }
        str++;
    }

    // Special case: If looking for the null terminator, return the pointer to it
    if (target == '\0') {
        return (char*)str;
    }

    // Returns the address of the last match found, or NULL if it never matched
    return last_match;
}