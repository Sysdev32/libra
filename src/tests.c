#include <drivers/hvfs.h>
#include <drivers/fb.h>
void balright() {
    printk(LOG_NONE, "just a tiny: balright\n");
}
#include <drivers/hvfs.h>
#include <drivers/fb.h> // For printk

#define HVFS_MAX_CHILDREN_BUF 512

/**
 * @brief Helper function to print tree nodes recursively
 */
static void hvfs_print_tree_recursive(const char *path, int depth) {
    char list_buf[HVFS_MAX_CHILDREN_BUF];
    
    // Get list of direct children at current path
    int child_count = hvfs_listdir(path, list_buf, sizeof(list_buf));
    if (child_count <= 0) {
        return;
    }

    char *p = list_buf;
    while (*p != '\0') {
        // Extract next entry name (delimited by \n)
        char child_name[HVFS_MAX_NAME_LEN];
        size_t idx = 0;
        
        while (*p != '\0' && *p != '\n' && idx < sizeof(child_name) - 1) {
            child_name[idx++] = *p++;
        }
        child_name[idx] = '\0';

        if (*p == '\n') p++; // Skip the newline

        if (idx == 0) continue;

        // Print indentation tree prefix
        for (int i = 0; i < depth; i++) {
            printk(LOG_NONE, "    ");
        }
        printk(LOG_NONE, "|-- %s", child_name);

        // Build child's full path for recursive traversal
        char child_path[256];
        size_t path_len = strlen(path);
        
        // Handle root "/" path vs subpaths safely
        if (path_len == 1 && path[0] == '/') {
            // Path is root "/"
            child_path[0] = '/';
            child_path[1] = '\0';
            strncat(child_path, child_name, sizeof(child_path) - 2);
        } else {
            // Path is a subfolder, e.g. "/RNKL"
            strncpy(child_path, path, sizeof(child_path) - 1);
            child_path[sizeof(child_path) - 1] = '\0';
            strncat(child_path, "/", sizeof(child_path) - strlen(child_path) - 1);
            strncat(child_path, child_name, sizeof(child_path) - strlen(child_path) - 1);
        }

        // Print node type indicator if applicable
        hvfs_type_t type = HVFS_TYPE_UNDEFINED;
        if (hvfs_get_type(child_path, &type) == 0 && type != HVFS_TYPE_UNDEFINED) {
            switch (type) {
                case HVFS_TYPE_FUNCTION: printk(LOG_NONE, " [func]"); break;
                case HVFS_TYPE_STRING:   printk(LOG_NONE, " [str]"); break;
                case HVFS_TYPE_U8:
                case HVFS_TYPE_U16:
                case HVFS_TYPE_U32:
                case HVFS_TYPE_U64:      printk(LOG_NONE, " [int]"); break;
                case HVFS_TYPE_BINARY:   printk(LOG_NONE, " [bin]"); break;
                default: break;
            }
        }
        printk(LOG_NONE, "\n");

        // Recurse into the subkey
        hvfs_print_tree_recursive(child_path, depth + 1);
    }
}

/**
 * @brief Public entry point for viewing the tree starting from root_path
 */
void hvfs_tree(const char *root_path) {
    printk(LOG_NONE, "%s\n", root_path);
    hvfs_print_tree_recursive(root_path, 0);
}
void tests() {
    hvfs_init();
    hvfs_create("/RNKL/Tests/balright"); // Note: missing parent "/RNKL/Tests" is auto-created!
    hvfs_create("/RNKL/Version");
    hvfs_set_type("/RNKL/Tests/balright", HVFS_TYPE_FUNCTION);
    
    // 1. Declare a local variable holding the function pointer
    hvfs_func_t fn = balright;
    
    // 2. Pass the ADDRESS of fn, and use sizeof(hvfs_func_t) instead of hardcoded 8
    hvfs_set("/RNKL/Tests/balright", &fn, sizeof(hvfs_func_t));
    
    // 3. Invoke it
    hvfs_call("/RNKL/Tests/balright", NULL);
    hvfs_tree("/");
}