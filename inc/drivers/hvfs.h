#ifndef HVFS_H
#define HVFS_H

#include <stddef.h>
#include <string.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#include <stdint.h>
#else
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long      uintptr_t;
#endif

/* External Kernel Memory Allocators */
extern void *kmalloc(size_t size);
extern void  kfree(void *ptr);

/* Standard POSIX Error Codes */
#ifndef ENOENT
#define ENOENT  2   /* Key not found */
#endif
#ifndef ENOMEM
#define ENOMEM  12  /* Out of memory */
#endif
#ifndef EINVAL
#define EINVAL  22  /* Invalid argument */
#endif
#ifndef EEXIST
#define EEXIST  17  /* Key already exists */
#endif

/* Maximum length for a single path component (key name) */
#define HVFS_MAX_NAME_LEN 64

/* HVFS Types */
typedef enum {
    HVFS_TYPE_UNDEFINED = 0,
    HVFS_TYPE_U8,
    HVFS_TYPE_U16,
    HVFS_TYPE_U32,
    HVFS_TYPE_U64,
    HVFS_TYPE_STRING,
    HVFS_TYPE_BINARY,
    HVFS_TYPE_FUNCTION
} hvfs_type_t;

/* Function Pointer Signature */
typedef int (*hvfs_func_t)(void *args);

/* HVFS Node Structure */
typedef struct hvfs_node {
    char name[HVFS_MAX_NAME_LEN];
    hvfs_type_t type;
    
    void *value;
    size_t size;

    struct hvfs_node *parent;
    struct hvfs_node *first_child;
    struct hvfs_node *next_sibling;
} hvfs_node_t;

/* --- API Functions --- */

/**
 * @brief Initialize the HVFS tree root. Must be called once before use.
 * @return 0 on success, -ENOMEM on failure.
 */
int hvfs_init(void);

/**
 * @brief Creates a key at the specified path. Missing parent keys are created automatically.
 * Newly created keys have type HVFS_TYPE_UNDEFINED.
 */
int hvfs_create(const char *path);

/**
 * @brief Sets the type of an existing key. Discards previous value and zeroes storage.
 */
int hvfs_set_type(const char *path, hvfs_type_t type);

/**
 * @brief Retrieves the current type of a key.
 */
int hvfs_get_type(const char *path, hvfs_type_t *type);

/**
 * @brief Sets the value of a key (including function pointers if type is HVFS_TYPE_FUNCTION).
 * Type must NOT be HVFS_TYPE_UNDEFINED.
 */
int hvfs_set(const char *path, const void *buffer, size_t size);

/**
 * @brief Gets the value of a key into the provided buffer.
 * Type must NOT be HVFS_TYPE_UNDEFINED.
 */
int hvfs_get(const char *path, void *buffer, size_t size);

/**
 * @brief Removes a key and recursively destroys all descendant keys.
 */
int hvfs_remove(const char *path);

/**
 * @brief Invokes the registered function stored at a key path.
 * Returns -EINVAL if the key is not HVFS_TYPE_FUNCTION or has no function set.
 */
int hvfs_call(const char *path, void *args);

/**
 * @brief Lists the names of direct child keys for a path into a buffer separated by '\n'.
 * @param path The key path to list.
 * @param buffer Output buffer to receive newline-separated key names.
 * @param max_len Maximum capacity of buffer.
 * @return Number of children found on success, or negative error code on failure (-ENOENT, -EINVAL).
 */
int hvfs_listdir(const char *path, char *buffer, size_t max_len);
typedef struct hvfs_stat {
    hvfs_type_t type;
    size_t size;
    size_t child_count;
    int has_value;
} hvfs_stat_t;

/**
 * @brief Retrieves metadata and status information for a given key path.
 * @param path The key path to stat.
 * @param statbuf Pointer to an hvfs_stat_t structure to receive metadata.
 * @return 0 on success, or a negative error code (-EINVAL, -ENOENT).
 */
int hvfs_stat(const char *path, hvfs_stat_t *statbuf);
#endif /* HVFS_H */