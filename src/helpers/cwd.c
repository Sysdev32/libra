#include <stdint.h>
#include <stddef.h>
#include <fs/mnt.h>
#define PATH_MAX 512
int canonicalize_path(const char *input, char *out, size_t out_size) {
    char segments[64][64]; // Max depth 64, max segment length 64
    int depth = 0;
    size_t idx = 0;

    // Split input into components
    while (input[idx] != '\0') {
        // Skip consecutive slashes
        while (input[idx] == '/') idx++;
        if (input[idx] == '\0') break;

        // Extract segment
        size_t seg_len = 0;
        char seg[64];
        while (input[idx] != '\0' && input[idx] != '/') {
            if (seg_len < sizeof(seg) - 1) {
                seg[seg_len++] = input[idx];
            }
            idx++;
        }
        seg[seg_len] = '\0';

        // Evaluate segment
        if (seg[0] == '.' && seg[1] == '\0') {
            // "." -> stay in current directory
            continue;
        } else if (seg[0] == '.' && seg[1] == '.' && seg[2] == '\0') {
            // ".." -> move up one level
            if (depth > 0) {
                depth--;
            }
        } else {
            // Regular directory name -> push to stack
            if (depth < 64) {
                strcpy(segments[depth], seg);
                depth++;
            } else {
                return -1; // Path too deep
            }
        }
    }

    // Reconstruct normalized absolute path
    if (depth == 0) {
        if (out_size < 2) return -1;
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }

    size_t pos = 0;
    for (int i = 0; i < depth; i++) {
        if (pos + 1 >= out_size) return -1;
        out[pos++] = '/';

        size_t slen = strlen(segments[i]);
        if (pos + slen >= out_size) return -1;
        
        memcpy(out + pos, segments[i], slen);
        pos += slen;
    }
    out[pos] = '\0';

    return 0;
}

// --- Freestanding chdir & getcwd ---

int chdir(const char *path) {
    char* current_cwd = getpcwd();
    if (!path || path[0] == '\0') return -1;

    char combined[PATH_MAX];
    char canonical[PATH_MAX];

    // 1. Resolve relative vs absolute path
    if (path[0] == '/') {
        // Absolute path
        if (strlen(path) >= PATH_MAX) return -1;
        strcpy(combined, path);
    } else {
        // Relative path: prepend CWD
        size_t cwd_len = strlen(current_cwd);
        size_t path_len = strlen(path);

        if (cwd_len + 1 + path_len >= PATH_MAX) return -1;

        strcpy(combined, current_cwd);
        if (combined[cwd_len - 1] != '/') {
            combined[cwd_len] = '/';
            combined[cwd_len + 1] = '\0';
        }
        
        // Append relative path
        size_t end = strlen(combined);
        strcpy(combined + end, path);
    }

    // 2. Canonicalize '.', '..', and extra slashes
    if (canonicalize_path(combined, canonical, PATH_MAX) != 0) {
        return -1;
    }
    struct vfs_stat st;
    if (vfs_stat(canonical, &st) == -1) {
        return -1;
    }
    if (!((st.st_mode & 0170000) == 0040000)) {
        return -1;
    }
    // 4. Update working directory
    strcpy(current_cwd, canonical);
    return 0;
}

char *getcwd(char *buf, size_t size) {
    char* current_cwd = getpcwd();
    if (!buf) return 0;

    size_t len = strlen(current_cwd) + 1;
    if (size < len) return 0; // Buffer too small

    memcpy(buf, current_cwd, len);
    return buf;
}