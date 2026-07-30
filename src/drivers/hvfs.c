#include <drivers/hvfs.h>
#include <hals/ahci.h>
/* Root node of the HVFS hierarchy */
static hvfs_node_t *g_root = NULL;

/* --- Private Helpers --- */

static size_t hvfs_get_expected_type_size(hvfs_type_t type) {
    switch (type) {
        case HVFS_TYPE_U8:       return sizeof(uint8_t);
        case HVFS_TYPE_U16:      return sizeof(uint16_t);
        case HVFS_TYPE_U32:      return sizeof(uint32_t);
        case HVFS_TYPE_U64:      return sizeof(uint64_t);
        case HVFS_TYPE_FUNCTION: return sizeof(hvfs_func_t);
        default:                 return 0; /* Dynamic sizes (STRING, BINARY) or UNDEFINED */
    }
}

static hvfs_node_t *hvfs_allocate_node(const char *name) {
    hvfs_node_t *node = (hvfs_node_t *)kmalloc(sizeof(hvfs_node_t));
    if (!node) return NULL;

    memset(node, 0, sizeof(hvfs_node_t));
    strncpy(node->name, name, HVFS_MAX_NAME_LEN - 1);
    node->name[HVFS_MAX_NAME_LEN - 1] = '\0';
    node->type = HVFS_TYPE_UNDEFINED;

    return node;
}

static hvfs_node_t *hvfs_find_child(hvfs_node_t *parent, const char *name, size_t name_len) {
    if (!parent) return NULL;

    hvfs_node_t *curr = parent->first_child;
    while (curr) {
        if (strlen(curr->name) == name_len && strncmp(curr->name, name, name_len) == 0) {
            return curr;
        }
        curr = curr->next_sibling;
    }
    return NULL;
}

static void hvfs_add_child(hvfs_node_t *parent, hvfs_node_t *child) {
    child->parent = parent;
    child->next_sibling = NULL;

    if (!parent->first_child) {
        parent->first_child = child;
    } else {
        hvfs_node_t *curr = parent->first_child;
        while (curr->next_sibling) {
            curr = curr->next_sibling;
        }
        curr->next_sibling = child;
    }
}

static hvfs_node_t *hvfs_lookup_path(const char *path) {
    if (!g_root || !path || path[0] != '/') return NULL;

    /* Root path "/" */
    if (path[1] == '\0') return g_root;

    hvfs_node_t *curr = g_root;
    const char *p = path + 1;

    while (*p != '\0') {
        const char *start = p;
        while (*p != '/' && *p != '\0') {
            p++;
        }

        size_t segment_len = (size_t)(p - start);
        if (segment_len == 0) {
            if (*p == '/') { p++; continue; } /* Handle double slashes */
            break;
        }

        curr = hvfs_find_child(curr, start, segment_len);
        if (!curr) return NULL;

        if (*p == '/') p++;
    }

    return curr;
}

static void hvfs_free_value(hvfs_node_t *node) {
    if (node->value) {
        kfree(node->value);
        node->value = NULL;
    }
    node->size = 0;
}

static void hvfs_destroy_node_recursive(hvfs_node_t *node) {
    if (!node) return;

    hvfs_node_t *child = node->first_child;
    while (child) {
        hvfs_node_t *next = child->next_sibling;
        hvfs_destroy_node_recursive(child);
        child = next;
    }

    hvfs_free_value(node);
    kfree(node);
}

/* --- Public API Implementation --- */

int hvfs_init(void) {
    if (g_root) return 0;

    g_root = hvfs_allocate_node("");
    if (!g_root) return -ENOMEM;

    return 0;
}

int hvfs_create(const char *path) {
    if (!g_root && hvfs_init() != 0) return -ENOMEM;
    if (!path || path[0] != '/') return -EINVAL;

    if (path[1] == '\0') return 0; /* Root already exists */

    hvfs_node_t *curr = g_root;
    const char *p = path + 1;

    while (*p != '\0') {
        const char *start = p;
        while (*p != '/' && *p != '\0') {
            p++;
        }

        size_t segment_len = (size_t)(p - start);
        if (segment_len == 0 || segment_len >= HVFS_MAX_NAME_LEN) {
            return -EINVAL;
        }

        hvfs_node_t *child = hvfs_find_child(curr, start, segment_len);
        if (!child) {
            char name_buf[HVFS_MAX_NAME_LEN];
            memcpy(name_buf, start, segment_len);
            name_buf[segment_len] = '\0';

            child = hvfs_allocate_node(name_buf);
            if (!child) return -ENOMEM;

            hvfs_add_child(curr, child);
        }

        curr = child;
        if (*p == '/') p++;
    }

    return 0;
}

int hvfs_set_type(const char *path, hvfs_type_t type) {
    hvfs_node_t *node = hvfs_lookup_path(path);
    if (!node) return -ENOENT;

    hvfs_free_value(node);
    node->type = type;

    return 0;
}

int hvfs_get_type(const char *path, hvfs_type_t *type) {
    if (!type) return -EINVAL;

    hvfs_node_t *node = hvfs_lookup_path(path);
    if (!node) return -ENOENT;

    *type = node->type;
    return 0;
}

int hvfs_set(const char *path, const void *buffer, size_t size) {
    if (!buffer) return -EINVAL;

    hvfs_node_t *node = hvfs_lookup_path(path);
    if (!node) return -ENOENT;
    if (node->type == HVFS_TYPE_UNDEFINED) return -EINVAL;

    /* Fixed-size types validation (U8, U16, U32, U64, FUNCTION) */
    size_t expected_size = hvfs_get_expected_type_size(node->type);
    if (expected_size != 0 && size != expected_size) {
        return -EINVAL;
    }

    if (node->type == HVFS_TYPE_STRING) {
        if (size == 0 || ((const char *)buffer)[size - 1] != '\0') {
            return -EINVAL;
        }
    }

    /* Allocate buffer for new data */
    void *new_val = kmalloc(size);
    if (!new_val) return -ENOMEM;

    memcpy(new_val, buffer, size);

    hvfs_free_value(node);
    node->value = new_val;
    node->size = size;

    return 0;
}

int hvfs_get(const char *path, void *buffer, size_t size) {
    if (!buffer) return -EINVAL;

    hvfs_node_t *node = hvfs_lookup_path(path);
    if (!node) return -ENOENT;
    if (node->type == HVFS_TYPE_UNDEFINED) return -EINVAL;
    if (!node->value || node->size != size) return -EINVAL;

    memcpy(buffer, node->value, size);
    return 0;
}

int hvfs_remove(const char *path) {
    hvfs_node_t *node = hvfs_lookup_path(path);
    if (!node) return -ENOENT;
    if (node == g_root) return -EINVAL; /* Root cannot be removed */

    /* Unlink from parent's child list */
    hvfs_node_t *parent = node->parent;
    if (parent) {
        if (parent->first_child == node) {
            parent->first_child = node->next_sibling;
        } else {
            hvfs_node_t *prev = parent->first_child;
            while (prev && prev->next_sibling != node) {
                prev = prev->next_sibling;
            }
            if (prev) {
                prev->next_sibling = node->next_sibling;
            }
        }
    }

    hvfs_destroy_node_recursive(node);
    return 0;
}
// CVE-2026-0001 (fixed)
int hvfs_call(const char *path, void *args) {
    hvfs_node_t *node = hvfs_lookup_path(path);
    if (!node) return -ENOENT;
    
    if (node->type != HVFS_TYPE_FUNCTION || !node->value || node->size != sizeof(hvfs_func_t)) {
        return -EINVAL;
    }
    if ((uint64_t)node->value > HHDM_OFFSET) {
        hvfs_func_t func = *(hvfs_func_t *)node->value;
        if (!func) return -EINVAL;

        return func(args);
    } else {
        return -EINVAL;
    }
}

int hvfs_listdir(const char *path, char *buffer, size_t max_len) {
    if (!buffer || max_len == 0) return -EINVAL;

    hvfs_node_t *node = hvfs_lookup_path(path);
    if (!node) return -ENOENT;

    buffer[0] = '\0';
    size_t written = 0;
    int child_count = 0;

    hvfs_node_t *child = node->first_child;
    while (child) {
        size_t name_len = strlen(child->name);
        
        /* Check if name + newline + null terminator fits */
        if (written + name_len + 2 > max_len) {
            return -EINVAL; /* Buffer size exceeded */
        }

        memcpy(buffer + written, child->name, name_len);
        written += name_len;
        buffer[written++] = '\n';
        buffer[written] = '\0';

        child_count++;
        child = child->next_sibling;
    }

    return child_count;
}
int hvfs_stat(const char *path, hvfs_stat_t *statbuf) {
    if (!statbuf) return -EINVAL;

    hvfs_node_t *node = hvfs_lookup_path(path);
    if (!node) return -ENOENT;

    /* Count direct children */
    size_t count = 0;
    hvfs_node_t *child = node->first_child;
    while (child) {
        count++;
        child = child->next_sibling;
    }

    statbuf->type = node->type;
    statbuf->size = node->size;
    statbuf->child_count = count;
    statbuf->has_value = (node->value != NULL) ? 1 : 0;

    return 0;
}