#include <stdio.h>
#include <stdlib.h>
#include <libtcc.h>
#include <unistd.h>
#include <fcntl.h>
#include <carrera.h>
#include <string.h> 
#include <sys/stat.h>
#include <sha256.h>
#include <sys/utsname.h>

char active_user[64] = "";
int shift_active = 0;
static const short shift_keymap[128] = {
    [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$',
    [0x06]='%', [0x07]='^', [0x08]='&', [0x09]='*',
    [0x0A]='(', [0x0B]=')',
    [0x0C]='_', [0x0D]='+',

    [0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',[0x14]='T',
    [0x15]='Y',[0x16]='U',[0x17]='I',[0x18]='O',[0x19]='P',
    [0x1A]='{',[0x1B]='}',
    [0x1E]='A',[0x1F]='S',[0x20]='D',[0x21]='F',[0x22]='G',
    [0x23]='H',[0x24]='J',[0x25]='K',[0x26]='L',
    [0x27]=':', [0x28]='"', [0x29]='~',
    [0x2B]='|',
    [0x2C]='Z',[0x2D]='X',[0x2E]='C',[0x2F]='V',
    [0x30]='B',[0x31]='N',[0x32]='M',
    [0x33]='<',[0x34]='>',[0x35]='?',

    [0x39]=' '
};
typedef struct {
    char username[64];
    uint32_t uid;
    uint32_t gid;
    char home[256];
    uint32_t shadow_id;
} passwd_entry_t;

typedef struct {
    uint32_t shadow_id;
    char hash[65];
} shadow_entry_t;

int parse_passwd_file(const char *contents, passwd_entry_t *entries, size_t max_entries) {
    if (!contents || !entries || max_entries == 0) return -1;
    size_t count = 0;
    const char *p = contents;
    while (*p && count < max_entries) {
        char line[512];
        size_t len = 0;
        while (*p && *p != '\n' && len < sizeof(line) - 1) line[len++] = *p++;
        line[len] = '\0';
        if (*p == '\n') p++;
        if (len == 0) continue;

        passwd_entry_t *e = &entries[count];
        char *field, *save;

        field = strtok_r(line, ":", &save);
        if (!field) continue;
        strncpy(e->username, field, sizeof(e->username) - 1);
        e->username[sizeof(e->username) - 1] = '\0';

        field = strtok_r(NULL, ":", &save);
        if (!field) continue;
        e->uid = atoi(field);

        field = strtok_r(NULL, ":", &save);
        if (!field) continue;
        e->gid = atoi(field);

        field = strtok_r(NULL, ":", &save);
        if (!field) continue;
        strncpy(e->home, field, sizeof(e->home) - 1);
        e->home[sizeof(e->home) - 1] = '\0';

        field = strtok_r(NULL, ":", &save);
        if (!field) continue;
        e->shadow_id = atoi(field);

        count++;
    }
    return (int)count;
}

int parse_shadow_file(const char *contents, shadow_entry_t *entries, size_t max_entries) {
    if (!contents || !entries || max_entries == 0) return -1;
    size_t count = 0;
    const char *p = contents;
    while (*p && count < max_entries) {
        char line[256];
        size_t len = 0;
        while (*p && *p != '\n' && len < sizeof(line) - 1) line[len++] = *p++;
        line[len] = '\0';
        if (*p == '\n') p++;
        if (len == 0) continue;

        shadow_entry_t *e = &entries[count];
        char *field, *save;

        field = strtok_r(line, ":", &save);
        if (!field) continue;
        e->shadow_id = atoi(field);

        field = strtok_r(NULL, ":", &save);
        if (!field) continue;
        strncpy(e->hash, field, sizeof(e->hash) - 1);
        e->hash[sizeof(e->hash) - 1] = '\0';

        count++;
    }
    return (int)count;
}

int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

#define KEY_UP_ARROW    -2
#define KEY_DOWN_ARROW  -3

static const short keymap[128] = {
    [0x00] = 0,
    [0x01] = 27,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b',
    [0x0F] = '\t',

    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',
    [0x1D] = 0,

    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x29] = '`',
    [0x2A] = 0,
    [0x2B] = '\\',

    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x36] = 0,
    [0x37] = '*',
    [0x38] = 0,
    [0x39] = ' ',
    [0x3A] = 0,

    [0x48] = KEY_UP_ARROW,
    [0x4B] = '4', [0x4C] = '5', [0x4D] = '6',
    [0x4F] = '1', 
    [0x50] = KEY_DOWN_ARROW,
    [0x51] = '3',
    [0x52] = '0', [0x53] = '.'
};

#define MAX_ARGS 64
typedef enum {
    num,
    str
} envtype;

typedef struct {
    char name[64];
    envtype type;
    union {
        long num;
        char str[512];
    } value;
} env_t;

env_t envs[32] = {0};
int lenv = 0;

static const char *get_env_value(const char *name)
{
    static char numbuf[32];

    for (int i = 0; i < lenv; i++) {
        if (strcmp(envs[i].name, name) == 0) {
            if (envs[i].type == str)
                return envs[i].value.str;

            snprintf(numbuf, sizeof(numbuf), "%ld", envs[i].value.num);
            return numbuf;
        }
    }

    return "";
}

void expand_env_vars(const char *input, char *output, size_t max_len) {
    size_t i = 0, j = 0;
    int in_single_quote = 0;
    int in_double_quote = 0;

    while (input[i] && j < max_len - 1) {
        if (input[i] == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            output[j++] = input[i++];
        } else if (input[i] == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            output[j++] = input[i++];
        } else if (input[i] == '$' && !in_single_quote) {
            i++;
            char var_name[64];
            size_t v = 0;
            while (input[i] && v < sizeof(var_name) - 1 && 
                   ((input[i] >= 'A' && input[i] <= 'Z') || 
                    (input[i] >= 'a' && input[i] <= 'z') || 
                    (input[i] >= '0' && input[i] <= '9') || 
                    input[i] == '_')) {
                var_name[v++] = input[i++];
            }
            var_name[v] = '\0';
            
            if (v > 0) {
                const char *val = get_env_value(var_name);
                while (*val && j < max_len - 1) {
                    output[j++] = *val++;
                }
            } else {
                if (j < max_len - 1) {
                    output[j++] = '$';
                }
            }
        } else {
            output[j++] = input[i++];
        }
    }
    output[j] = '\0';
}

int tokenize(char *line, char *argv[]) {
    int argc = 0;
    while (*line) {
        while (*line == ' ' || *line == '\t' || *line == '\n' || *line == '\r') {
            line++;
        }
        if (*line == '\0') break;
        if (argc >= MAX_ARGS - 1) break;

        if (*line == '"' || *line == '\'') {
            char quote = *line++;
            argv[argc++] = line;
            while (*line && *line != quote) line++;
            if (*line == quote) {
                *line = '\0';
                line++;
            }
        } else {
            argv[argc++] = line;
            while (*line && *line != ' ' && *line != '\t' && *line != '\n' && *line != '\r') {
                line++;
            }
            if (*line) {
                *line = '\0';
                line++;
            }
        }
    }
    argv[argc] = NULL;
    return argc;
}

int path_absolute(const char *cwd, const char *path, char *out, size_t out_size) {
    size_t len = 0;
    if (path[0] == '/') {
        if (len + 1 >= out_size) return -1;
        out[len++] = '/';
        path++;
    } else {
        while (*cwd) {
            if (len + 1 >= out_size) return -1;
            out[len++] = *cwd++;
        }
        if (len == 0) {
            if (len + 1 >= out_size) return -1;
            out[len++] = '/';
        } else if (out[len - 1] != '/') {
            if (len + 1 >= out_size) return -1;
            out[len++] = '/';
        }
    }

    while (*path) {
        char part[256];
        size_t plen = 0;
        while (*path == '/') path++;
        while (*path && *path != '/' && plen < sizeof(part) - 1) {
            part[plen++] = *path++;
        }
        part[plen] = '\0';

        if (plen == 0) continue;
        if (strcmp(part, ".") == 0) continue;
        if (strcmp(part, "..") == 0) {
            if (len > 1) {
                len--;
                while (len > 1 && out[len - 1] != '/') len--;
                out[len] = '\0';
            }
            continue;
        }

        if (len > 1 && out[len - 1] != '/') {
            if (len + 1 >= out_size) return -1;
            out[len++] = '/';
        }

        for (size_t i = 0; i < plen; i++) {
            if (len + 1 >= out_size) return -1;
            out[len++] = part[i];
        }
        out[len] = '\0';
    }

    if (len == 0) {
        if (out_size < 2) return -1;
        out[0] = '/';
        out[1] = '\0';
    } else {
        out[len] = '\0';
    }
    return 0;
}

static const char *builtin_commands[] = {
    "echo", "cd", "ls", "uname", "hostname", "touch", 
    "cat", "mkdir", "rmdir", "reboot", "poweroff", "rm", "pwd", "sh", "export"
};
#define NUM_BUILTINS (sizeof(builtin_commands) / sizeof(char *))

#define HISTORY_SIZE 16
char history[HISTORY_SIZE][256];
int history_count = 0;
int history_index = -1;

void add_to_history(const char *cmd) {
    if (strlen(cmd) == 0) return;
    if (history_count > 0 && strcmp(history[(history_count - 1) % HISTORY_SIZE], cmd) == 0) {
        return;
    }
    strncpy(history[history_count % HISTORY_SIZE], cmd, 255);
    history_count++;
}

void execute_command(int argc, char *argv[], char *pathbuf, const char *homebuf, const char *hostnamebuf, const char *prompt_path, const char *active_user);

void run_shell_script(const char *script_path, char *pathbuf, const char *homebuf, const char *hostnamebuf, const char *prompt_path, const char *active_user) {
    int fd = open(script_path, O_RDONLY);
    if (fd < 0) {
        printf("csh: script not found: %s\n", script_path);
        return;
    }

    char buffer[4096];
    int bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (bytes_read <= 0) return;
    buffer[bytes_read] = '\0';

    char *line = buffer;
    char *next_line;

    while (line && *line) {
        next_line = strchr(line, '\n');
        if (next_line) {
            *next_line = '\0';
            next_line++;
        }

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r') {
            line[len - 1] = '\0';
        }

        while (*line == ' ' || *line == '\t') {
            line++;
        }

        if (*line != '\0' && *line != '#') {
            char line_copy[256];
            strncpy(line_copy, line, sizeof(line_copy) - 1);
            line_copy[sizeof(line_copy) - 1] = '\0';

            char expanded_line[1024];
            expand_env_vars(line_copy, expanded_line, sizeof(expanded_line));

            char *script_argv[MAX_ARGS];
            int script_argc = tokenize(expanded_line, script_argv);

            if (script_argc > 0 && script_argv[0] != NULL) {
                execute_command(script_argc, script_argv, pathbuf, homebuf, hostnamebuf, prompt_path, active_user);
            }
        }
        line = next_line;
    }
}

void help(char *name)
{
    if (!strcmp(name, "echo")) {
        printf("echo - Echo text to the terminal\nUsage: echo <text>\n");
    }
    else if (!strcmp(name, "cd")) {
        printf("cd - Change current directory\nUsage: cd [directory]\nIf no directory is provided, goes to home.\n");
    }
    else if (!strcmp(name, "ls")) {
        printf("ls - List directory contents\nUsage: ls [path]\n");
    }
    else if (!strcmp(name, "uname")) {
        printf("uname - Print system information\nUsage: uname [-a]\n");
    }
    else if (!strcmp(name, "hostname")) {
        printf("hostname - Get or set system hostname\nUsage: hostname [name]\n");
    }
    else if (!strcmp(name, "touch")) {
        printf("touch - Create an empty file\nUsage: touch <file-path>\n");
    }
    else if (!strcmp(name, "cat")) {
        printf("cat - Print file contents\nUsage: cat <file-path>\n");
    }
    else if (!strcmp(name, "mkdir")) {
        printf("mkdir - Create a directory\nUsage: mkdir <directory-path>\n");
    }
    else if (!strcmp(name, "rmdir")) {
        printf("rmdir - Remove a directory\nUsage: rmdir <directory-path>\n");
    }
    else if (!strcmp(name, "rm")) {
        printf("rm - Remove a file\nUsage: rm <file>\n");
    }
    else if (!strcmp(name, "pwd")) {
        printf("pwd - Print current working directory\nUsage: pwd\n");
    }
    else if (!strcmp(name, "sh")) {
        printf("sh - Execute a shell script\nUsage: sh <script.sh>\n");
    }
    else if (!strcmp(name, "whoami")) {
        printf("whoami - Print current user\nUsage: whoami\n");
    }
    else if (!strcmp(name, "reboot")) {
        printf("reboot - Restart the system\nUsage: reboot\n");
    }
    else if (!strcmp(name, "poweroff")) {
        printf("poweroff - Shut down the system\nUsage: poweroff\n");
    }
    else if (!strcmp(name, "export")) {
        printf("export - Set an environment variable\nUsage: export VAR=value\n");
    }
    else if (!strcmp(name, "help")) {
        printf("help - Show command help\nUsage: help [command]\n");
    }
    else {
        printf("help: no help available for '%s'\n", name);
    }
}

void execute_command(int argc, char *argv[], char *pathbuf, const char *homebuf, const char *hostnamebuf, const char *prompt_path, const char *active_user) {
    if (strcmp(argv[0], "echo") == 0) {
        if (argv[1]) {
            printf("%s\n", argv[1]);
        } else {
            printf("\n");
        }
    } 
    else if (strcmp(argv[0], "export") == 0) {
        if (argc < 2) {
            for (int i = 0; i < lenv; i++) {
                if (envs[i].type == str) {
                    printf("%s=%s\n", envs[i].name, envs[i].value.str);
                } else {
                    printf("%s=%ld\n", envs[i].name, envs[i].value.num);
                }
            }
        } else {
            char *eq = strchr(argv[1], '=');
            if (eq) {
                *eq = '\0';
                char *name = argv[1];
                char *val = eq + 1;
                
                if (*val == '"' || *val == '\'') {
                    char q = *val;
                    val++;
                    char *end = strchr(val, q);
                    if (end) *end = '\0';
                }

                int found = -1;
                for (int i = 0; i < lenv; i++) {
                    if (strcmp(envs[i].name, name) == 0) {
                        found = i;
                        break;
                    }
                }
                if (found == -1 && lenv < 32) {
                    found = lenv++;
                }
                if (found != -1) {
                    strncpy(envs[found].name, name, sizeof(envs[found].name) - 1);
                    envs[found].name[sizeof(envs[found].name) - 1] = '\0';
                    envs[found].type = str;
                    strncpy(envs[found].value.str, val, sizeof(envs[found].value.str) - 1);
                    envs[found].value.str[sizeof(envs[found].value.str) - 1] = '\0';
                }
            }
        }
    }
    else if (strcmp(argv[0], "cd") == 0) {
        if (argv[1] == NULL) {
            if (strlen(homebuf) > 0) {
                strcpy(pathbuf, homebuf);
            } else {
                strcpy(pathbuf, "/");
            }
        } else {
            char abs[256] = {0};
            if (argv[1][0] == '~') {
                char expanded_path[256] = {0};
                snprintf(expanded_path, sizeof(expanded_path), "%s%s", homebuf, argv[1] + 1);
                path_absolute(pathbuf, expanded_path, abs, sizeof(abs));
            } else {
                path_absolute(pathbuf, argv[1], abs, sizeof(abs));
            }

            char file_buffers[32][256] = {{0}};
            char *contents[32];
            for (int i = 0; i < 32; i++) contents[i] = file_buffers[i];

            int count = listdir(abs, contents, 32);
            if (count >= 0) {
                strcpy(pathbuf, abs);
            } else {
                printf("cd: no such file or directory: %s\n", argv[1]);
            }
        }
    }
    else if (strcmp(argv[0], "ls") == 0) {
        char target_path[256] = {0};
        if (argv[1] != NULL) {
            if (argv[1][0] == '~') {
                char expanded_path[256] = {0};
                snprintf(expanded_path, sizeof(expanded_path), "%s%s", homebuf, argv[1] + 1);
                path_absolute(pathbuf, expanded_path, target_path, sizeof(target_path));
            } else {
                path_absolute(pathbuf, argv[1], target_path, sizeof(target_path));
            }
        } else {
            strcpy(target_path, pathbuf);
        }

        char file_buffers[32][256] = {{0}};
        char *contents[32];
        for (int i = 0; i < 32; i++) contents[i] = file_buffers[i];

        int count = listdir(target_path, contents, 32);
        if (count >= 0) {
            for (int i = 0; i < count; i++) {
                if (strcmp(contents[i], ".dir")) {
                    printf("%s ", contents[i]);
                }
            }
            printf("\n");
        } else {
            printf("ls: cannot access '%s': No such file or directory\n", argv[1] ? argv[1] : "");
        }
    } 
    else if (strcmp(argv[0], "uname") == 0) {
        if (argc == 1) {
            struct utsname uts;
            uname(&uts);
            printf("%s\n", uts.sysname);
        } else if (argc == 2 && (strcmp(argv[1], "-a") == 0)) {
            struct utsname uts;
            uname(&uts);
            printf("%s %s %s %s\n", uts.sysname, uts.nodename, uts.release, uts.version, uts.machine);
        }
    } 
    else if (strcmp(argv[0], "hostname") == 0) {
        if (argc == 1) {
            printf("%s\n", hostnamebuf);
        } else if (argc == 2) {
            sethostname(argv[1], strlen(argv[1]));
        }
    } 
    else if (strcmp(argv[0], "touch") == 0) {
        if (argc == 1) {
            printf("Usage: touch <file-path>\n");
        } else if (argc == 2) {
            char exec_path[256] = {0};
            if (argv[1][0] == '~') {
                char expanded_path[256] = {0};
                snprintf(expanded_path, sizeof(expanded_path), "%s%s", homebuf, argv[1] + 1);
                path_absolute(pathbuf, expanded_path, exec_path, sizeof(exec_path));
            } else {
                path_absolute(pathbuf, argv[1], exec_path, sizeof(exec_path));
            }
            createf("a", exec_path, 1);
        }
    } 
    else if (strcmp(argv[0], "cat") == 0) {
        if (argc != 2) {
            printf("Usage: cat <file-path>\n");
        } else {
            char exec_path[256] = {0};
            if (argv[1][0] == '~') {
                char expanded_path[256] = {0};
                snprintf(expanded_path, sizeof(expanded_path), "%s%s", homebuf, argv[1] + 1);
                path_absolute(pathbuf, expanded_path, exec_path, sizeof(exec_path));
            } else {
                path_absolute(pathbuf, argv[1], exec_path, sizeof(exec_path));
            }
            int fd = open(exec_path, O_RDONLY);
            if (fd < 0) {
                printf("cat: No such file or directory: %s\n", argv[1]);
                return;
            }

            char buf[512];
            int n_bytes;
            while ((n_bytes = read(fd, buf, sizeof(buf))) > 0) {
                fwrite(buf, 1, n_bytes, stdout);
            }
            close(fd);
            printf("\n");
        }
    } 
    else if (!strcmp(argv[0], "mkdir")) {
        char exec_path[256] = {0};
        if (argc != 2) {
            printf("Usage: mkdir <new_path>\n");
            return;
        }
        if (argv[1][0] == '~') {
            char expanded_path[256] = {0};
            snprintf(expanded_path, sizeof(expanded_path), "%s%s", homebuf, argv[1] + 1);
            path_absolute(pathbuf, expanded_path, exec_path, sizeof(exec_path));
        } else {
            path_absolute(pathbuf, argv[1], exec_path, sizeof(exec_path));
        }
        mkdir(exec_path, 0755);
    } 
    else if (!strcmp(argv[0], "rmdir")) {
        char exec_path[256] = {0};
        if (argc != 2) {
            printf("Usage: rmdir <folder_path>\n");
            return;
        }
        if (argv[1][0] == '~') {
            char expanded_path[256] = {0};
            snprintf(expanded_path, sizeof(expanded_path), "%s%s", homebuf, argv[1] + 1);
            path_absolute(pathbuf, expanded_path, exec_path, sizeof(exec_path));
        } else {
            path_absolute(pathbuf, argv[1], exec_path, sizeof(exec_path));
        }
        rmdir(exec_path);
    } 
    else if (!strcmp(argv[0], "reboot")) {
        reboot();
    } 
    else if (!strcmp(argv[0], "poweroff")) {
        poweroff();
    } 
    else if (!strcmp(argv[0], "rm")) {
        if (argc != 2) {
            printf("Usage: rm <file>\n");
            return;
        }
        char exec_path[256] = {0};
        if (argv[1][0] == '~') { 
            char expanded_path[256] = {0};
            snprintf(expanded_path, sizeof(expanded_path), "%s%s", homebuf, argv[1] + 1);
            path_absolute(pathbuf, expanded_path, exec_path, sizeof(exec_path));
        } else {
            path_absolute(pathbuf, argv[1], exec_path, sizeof(exec_path));
        }
        remove(exec_path);
    } 
    else if (!strcmp(argv[0], "pwd")) {
        printf("%s\n", pathbuf);
    } 
    else if (!strcmp(argv[0], "sh")) {
        if (argc != 2) {
            printf("Usage: sh <script.sh>\n");
            return;
        }
        char exec_path[256] = {0};
        if (argv[1][0] == '~') {
            char expanded_path[256] = {0};
            snprintf(expanded_path, sizeof(expanded_path), "%s%s", homebuf, argv[1] + 1);
            path_absolute(pathbuf, expanded_path, exec_path, sizeof(exec_path));
        } else {
            path_absolute(pathbuf, argv[1], exec_path, sizeof(exec_path));
        }
        run_shell_script(exec_path, pathbuf, homebuf, hostnamebuf, prompt_path, active_user);
    } else if (!strcmp(argv[0], "whoami")) {
        printf("%s\n", active_user);
    } else if (!strcmp(argv[0], "help")) {
        if (argc == 1) {
            printf("Commands:\nhelp whoami sh rm poweroff reboot echo cd export\npwd rmdir mkdir touch cat hostname uname ls\n");
        } else {
            help(argv[1]);
        }
    }
    else {
        char exec_path[256] = {0};
        int found_executable = 0;

        // If the command starts with '/' or '.' or '~', it contains a direct file path.
        if (argv[0][0] == '/' || argv[0][0] == '.' || argv[0][0] == '~') {
            if (argv[0][0] == '~') {
                char expanded_path[256] = {0};
                snprintf(expanded_path, sizeof(expanded_path), "%s%s", homebuf, argv[0] + 1);
                path_absolute(pathbuf, expanded_path, exec_path, sizeof(exec_path));
            } else {
                path_absolute(pathbuf, argv[0], exec_path, sizeof(exec_path));
            }
            if (path_exists(exec_path)) {
                found_executable = 1;
            }
        } 
        else {
            // No direct path context provided: only look in structural PATH variable configuration
            const char *path_env = get_env_value("PATH");
            if (strlen(path_env) > 0) {
                char path_env_copy[512];
                strncpy(path_env_copy, path_env, sizeof(path_env_copy) - 1);
                path_env_copy[sizeof(path_env_copy) - 1] = '\0';

                char *token;
                char *saveptr;
                token = strtok_r(path_env_copy, ":", &saveptr);
                while (token != NULL) {
                    char try_path[512];
                    snprintf(try_path, sizeof(try_path), "%s/%s", token, argv[0]);
                    
                    char resolved_path[256];
                    if (path_absolute(pathbuf, try_path, resolved_path, sizeof(resolved_path)) == 0) {
                        if (path_exists(resolved_path)) {
                            strcpy(exec_path, resolved_path);
                            found_executable = 1;
                            break;
                        }
                    }
                    token = strtok_r(NULL, ":", &saveptr);
                }
            }
        }

        if (found_executable) {
            size_t path_len = strlen(exec_path);
            if (path_len > 3 && strcmp(&exec_path[path_len - 3], ".sh") == 0) {
                run_shell_script(exec_path, pathbuf, homebuf, hostnamebuf, prompt_path, active_user);
            } else {
                spawn(exec_path, argc, argv);
            }
        } else {
            printf("csh: command not found: %s\n", argv[0]);
        }
    }
}

int main(void)
{
    printf("CSH\nOS: Carrera\nAuthor: Adam Ahmed\nVersion: 1.4\n");
    setvbuf(stdout, NULL, _IONBF, 0);
    
    strcpy(envs[0].name, "PATH");
    envs[0].type = str;
    strcpy(envs[0].value.str, "/System/usr/bin/commandline:/System/usr/bin/graphical:/System/usr/bin/bitype");
    lenv = 1;

    char pathbuf[256] = "/"; 
    char homebuf[256] = "";  
    passwd_entry_t p[32];
    shadow_entry_t s[32];

    int pfd = open("/etc/passwd", O_RDONLY);
    char bufn[512] = {0};
    int n = 0;
    if (pfd >= 0) {
        int bytes_read = 0;
        while (bytes_read < (int)sizeof(bufn) - 1 && (n = read(pfd, bufn + bytes_read, sizeof(bufn) - 1 - bytes_read)) > 0) {
            bytes_read += n;
        }
        bufn[bytes_read] = '\0';
        close(pfd);
    } else {
        printf("Could not open /etc/passwd\n");
        return 1;
    }

    int sfd = open("/etc/shadow", O_RDONLY);
    char bufs[512] = {0};
    int ns = 0;
    if (sfd >= 0) {
        int bytes_read = 0;
        while (bytes_read < (int)sizeof(bufs) - 1 && (ns = read(sfd, bufs + bytes_read, sizeof(bufs) - 1 - bytes_read)) > 0) {
            bytes_read += ns;
        }
        bufs[bytes_read] = '\0';
        close(sfd);
    } else {
        printf("Could not open /etc/shadow\n");
        return 1;
    }

    int nep = parse_passwd_file(bufn, p, 32);
    int nes = parse_shadow_file(bufs, s, 32);
    if (nep <= 0) {
        printf("Not enough users to continue!\n");
        return 1;
    }
    if (nes != nep) {
        printf("Invalid shadow/passwd configuration!\n");
        return 1;
    }

    int capslock_active = 0;
    int authenticated = 0;
    passwd_entry_t selected_user;
    memset(&selected_user, 0, sizeof(passwd_entry_t));

    while (1) {
        printf("\nAvailable users:\n");
        for (int i = 0; i < nep; i++) {
            printf("  - %s\n", p[i].username);
        }

        printf("Username: ");
        char typed_user[64] = {0};
        int uc = 0;

        while (1) {
            unsigned char c;
            if (read(0, &c, 1) <= 0) break;

            if (c == 0x3A) {
                capslock_active = !capslock_active;
                continue;
            }
            if (c == 0x2A) {
                shift_active = 1;
                continue;
            }
            if (c == (0x2A | 0x80)) {
                shift_active = 0;
                continue;
            }
            if (c == 0x36) {
                shift_active = 1;
                continue;
            }
            if (c == (0x36 | 0x80)) {
                shift_active = 0;
                continue;
            }
            if (c & 0x80) continue;

            short mapped;
            if (shift_active && shift_keymap[c])
                mapped = shift_keymap[c];
            else
                mapped = keymap[c];
            if (mapped <= 0) continue; 

            char ascii_char = (char)mapped;
            if (ascii_char == '\n') break;

            if (ascii_char == '\b') {
                if (uc > 0) {
                    uc--;
                    typed_user[uc] = '\0';
                    printf("\b \b");
                }
                continue;
            }

            if (capslock_active && (ascii_char >= 'a' && ascii_char <= 'z')) {
                ascii_char -= 32;
            }

            fputc(ascii_char, stdout);
            if (uc < 63) {
                typed_user[uc++] = ascii_char;
            }
        }
        printf("\n");

        int user_found = 0;
        for (int i = 0; i < nep; i++) {
            if (strcmp(p[i].username, typed_user) == 0) {
                selected_user = p[i];
                user_found = 1;
                break;
            }
        }

        if (user_found) {
            break;
        } else {
            printf("User '%s' not found. Try again.\n", typed_user);
        }
    }

    shadow_entry_t matched_shadow;
    memset(&matched_shadow, 0, sizeof(shadow_entry_t));
    for (int i = 0; i < nes; i++) {
        if (s[i].shadow_id == selected_user.shadow_id) {
            matched_shadow = s[i];
            break;
        }
    }

    int tries = 0;
    while (tries < 3) {
        SHA256_CTX ctx;
        sha256_init(&ctx);
        printf("%s passwd: ", selected_user.username);
        char line[256] = {0};
        int lc = 0;

        while (1) {
            unsigned char c;
            if (read(0, &c, 1) <= 0) break;

            if (c == 0x3A) {
                capslock_active = !capslock_active;
                continue;
            }
            if (c == 0x2A) {
                shift_active = 1;
                continue;
            }
            if (c == (0x2A | 0x80)) {
                shift_active = 0;
                continue;
            }
            if (c == 0x36) {
                shift_active = 1;
                continue;
            }
            if (c == (0x36 | 0x80)) {
                shift_active = 0;
                continue;
            }
            if (c & 0x80) continue;

            short mapped;
            if (shift_active && shift_keymap[c])
                mapped = shift_keymap[c];
            else
                mapped = keymap[c];
            if (mapped <= 0) continue; 

            char ascii_char = (char)mapped;
            if (ascii_char == '\n') break;

            if (ascii_char == '\b') {
                if (lc > 0) {
                    lc--;
                    line[lc] = '\0';
                    printf("\b \b");
                }
                continue;
            }

            if (capslock_active && (ascii_char >= 'a' && ascii_char <= 'z')) {
                ascii_char -= 32;
            }

            fputc('*', stdout);
            if (lc < 255) {
                line[lc++] = ascii_char;
            }
        }
        printf("\n");

        sha256_update(&ctx, (BYTE*)line, lc);
        BYTE hash[32] = {0};
        sha256_final(&ctx, hash);

        char hex_hash[65] = {0};
        for (int h = 0; h < 32; h++) {
            sprintf(&hex_hash[h * 2], "%02x", hash[h]);
        }

        if (strcmp(hex_hash, matched_shadow.hash) == 0) {
            authenticated = 1;
            break;
        }
        tries++;
    }

    if (!authenticated) {
        printf("Max tries: 3 reached\n");
        return 1;
    }

    strncpy(active_user, selected_user.username, sizeof(active_user) - 1);
    strncpy(homebuf, selected_user.home, sizeof(homebuf) - 1);
    if (strlen(homebuf) > 0) {
        strncpy(pathbuf, homebuf, sizeof(pathbuf) - 1);
    }

    while (1) {
        char hostnamebuf[256] = {0};
        gethostname(hostnamebuf, 256);

        char prompt_path[256] = {0};
        size_t home_len = strlen(homebuf);

        if (home_len > 0 && strncmp(pathbuf, homebuf, home_len) == 0) {
            if (pathbuf[home_len] == '\0' || pathbuf[home_len] == '/') {
                prompt_path[0] = '~';
                strcpy(prompt_path + 1, pathbuf + home_len);
            } else {
                strcpy(prompt_path, pathbuf);
            }
        } else {
            strcpy(prompt_path, pathbuf);
        }
        if (!strcmp(prompt_path, "~/")) {
            memset(prompt_path, 0, 256);
            prompt_path[0] = '~';
        }

        printf("%s@%s:%s$ ", active_user, hostnamebuf, prompt_path);
        
        char line[256] = {0};
        int lc = 0;
        history_index = history_count;

        while (1) {
            unsigned char c;
            if (read(0, &c, 1) <= 0) break;

            if (c == 0x3A) {
                capslock_active = !capslock_active;
                continue;
            }
            if (c == 0x2A) {
                shift_active = 1;
                continue;
            }
            if (c == (0x2A | 0x80)) {
                shift_active = 0;
                continue;
            }
            if (c == 0x36) {
                shift_active = 1;
                continue;
            }
            if (c == (0x36 | 0x80)) {
                shift_active = 0;
                continue;
            }
            if (c & 0x80) continue;

            short mapped;
            if (shift_active && shift_keymap[c])
                mapped = shift_keymap[c];
            else
                mapped = keymap[c];
            if (mapped == 0) continue;

            if (mapped == KEY_UP_ARROW) {
                if (history_count > 0 && history_index > 0) {
                    history_index--;
                    while (lc > 0) {
                        printf("\b \b");
                        lc--;
                    }
                    int index = history_index % HISTORY_SIZE;
                    strcpy(line, history[index]);
                    lc = strlen(line);
                    printf("%s", line);
                }
                continue;
            }

            if (mapped == KEY_DOWN_ARROW) {
                if (history_index < history_count) {
                    history_index++;
                    while (lc > 0) {
                        printf("\b \b");
                        lc--;
                    }
                    if (history_index == history_count) {
                        line[0] = '\0';
                        lc = 0;
                    } else {
                        int index = history_index % HISTORY_SIZE;
                        strcpy(line, history[index]);
                        lc = strlen(line);
                        printf("%s", line);
                    }
                }
                continue;
            }

            char ascii_char = (char)mapped;
            if (ascii_char == '\n') break;

            if (ascii_char == '\b') {
                if (lc > 0) {
                    lc--;
                    line[lc] = '\0';
                    printf("\b \b");
                }
                continue;
            }

            if (ascii_char == '\t') {
                line[lc] = '\0';
                char *last_space = strrchr(line, ' ');
                if (!last_space) {
                    int match_count = 0;
                    const char *matched_cmd = NULL;

                    for (size_t i = 0; i < NUM_BUILTINS; i++) {
                        if (strncmp(builtin_commands[i], line, lc) == 0) {
                            match_count++;
                            matched_cmd = builtin_commands[i];
                        }
                    }

                    if (match_count == 1) {
                        while (lc > 0) { printf("\b \b"); lc--; }
                        strcpy(line, matched_cmd);
                        strcat(line, " ");
                        lc = strlen(line);
                        printf("%s", line);
                    } else if (match_count > 1) {
                        printf("\n");
                        for (size_t i = 0; i < NUM_BUILTINS; i++) {
                            if (strncmp(builtin_commands[i], line, lc) == 0) {
                                printf("%s  ", builtin_commands[i]);
                            }
                        }
                        printf("\n%s@%s:%s$ %s", active_user, hostnamebuf, prompt_path, line);
                    }
                } else {
                    char *partial_path = last_space + 1;
                    char dir_to_search[256] = {0};
                    char file_prefix[256] = {0};

                    char *last_slash = strrchr(partial_path, '/');
                    if (last_slash) {
                        size_t dir_len = last_slash - partial_path + 1;
                        char temp_dir[256] = {0};
                        strncpy(temp_dir, partial_path, dir_len);
                        
                        if (temp_dir[0] == '~') {
                            snprintf(dir_to_search, sizeof(dir_to_search), "%s%s", homebuf, temp_dir + 1);
                        } else {
                            path_absolute(pathbuf, temp_dir, dir_to_search, sizeof(dir_to_search));
                        }
                        strcpy(file_prefix, last_slash + 1);
                    } else {
                        strcpy(dir_to_search, pathbuf);
                        strcpy(file_prefix, partial_path);
                    }

                    char file_buffers[32][256] = {{0}};
                    char *contents[32];
                    for (int i = 0; i < 32; i++) contents[i] = file_buffers[i];

                    int count = listdir(dir_to_search, contents, 32);
                    if (count > 0) {
                        int match_count = 0;
                        char matched_file[256] = {0};

                        for (int i = 0; i < count; i++) {
                            if (strcmp(contents[i], ".dir") != 0 &&
                                strncmp(contents[i], file_prefix, strlen(file_prefix)) == 0) {
                                match_count++;
                                strcpy(matched_file, contents[i]);
                            }
                        }

                        if (match_count == 1) {
                            size_t prefix_len = strlen(file_prefix);
                            while (prefix_len > 0) { printf("\b \b"); lc--; prefix_len--; }
                            
                            strcat(line, matched_file + strlen(file_prefix));
                            lc = strlen(line);
                            printf("%s", matched_file + strlen(file_prefix));
                        } else if (match_count > 1) {
                            printf("\n");
                            for (int i = 0; i < count; i++) {
                                if (strcmp(contents[i], ".dir") != 0 &&
                                    strncmp(contents[i], file_prefix, strlen(file_prefix)) == 0) {
                                    printf("%s  ", contents[i]);
                                }
                            }
                            printf("\n%s@%s:%s$ %s", active_user, hostnamebuf, prompt_path, line);
                        }
                    }
                }
                continue;
            }

            if (capslock_active && (ascii_char >= 'a' && ascii_char <= 'z')) {
                ascii_char -= 32;
            }

            fputc(ascii_char, stdout);
            if (lc < 255) {
                line[lc++] = ascii_char;
            }
        }
        printf("\n");
        line[lc] = '\0';

        add_to_history(line);

        char expanded_line[1024];
        expand_env_vars(line, expanded_line, sizeof(expanded_line));

        char *argv[MAX_ARGS];
        int argc = tokenize(expanded_line, argv);
        if (argc == 0 || argv[0] == NULL) {
            continue;
        }

        execute_command(argc, argv, pathbuf, homebuf, hostnamebuf, prompt_path, active_user);
    }
    return 0;
}
