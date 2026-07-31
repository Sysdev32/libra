# Syscalls of Libra

### Syscall 0: read
* **Signature:** `int read(int fd, void* buf, size_t count, u64 offset);`[cite: 1]
* **Implementation:** `unsigned long long sys_read(arg* a)`[cite: 1]
* **Description:** Checks if `fd` is standard stdout/stderr (1 or 2) returning 0, handles tracking index shifts for mount/device files, and executes VFS reads accordingly[cite: 1].

---

### Syscall 1: write
* **Signature:** `unsigned long long sys_write(arg* a);`[cite: 1]
* **Description:** Writes data through file descriptors, routing standard outputs (`1` and `2`) to serial ports and TTY char-by-char, or utilizing VFS write routines for other descriptors[cite: 1].

---

### Syscall 2: open
* **Signature:** `unsigned long long sys_open(arg* a);`[cite: 1]
* **Description:** Resolves user file paths against current working directories and opens either device endpoints or standard VFS files depending on path prefixes[cite: 1].

---

### Syscall 3: vfs_mkdir
* **Signature:** `unsigned long long sys_mkdir(arg* a);`[cite: 1]
* **Description:** Resolves target paths and invokes directory creation within the virtual file system[cite: 1].

---

### Syscall 4: vfs_rmdir
* **Signature:** `unsigned long long sys_rmdir(arg* a);`[cite: 1]
* **Description:** Resolves directory paths and removes them via VFS functions[cite: 1].

---

### Syscall 5: close
* **Signature:** `unsigned long long sys_close(arg* a);`[cite: 1]
* **Description:** Safely closes file descriptors, handling standard streams or releasing file tracking indices within the VFS layer[cite: 1].

---

### Syscall 6: vfs_move_file
* **Signature:** `unsigned long long sys_move_file(arg* a);`[cite: 1]
* **Description:** Relocates or renames an active file descriptor to a new resolved path destination[cite: 1].

---

### Syscall 7: create_file
* **Signature:** `unsigned long long sys_create_file(arg* a);`[cite: 1]
* **Description:** Creates files or special device nodes based on path resolution and manages tracking flags accordingly[cite: 1].

---

### Syscall 8: vfs_delete_file
* **Signature:** `unsigned long long sys_delete_file(arg* a);`[cite: 1]
* **Description:** Unlinks and deletes files from the VFS path location[cite: 1].

---

### Syscall 9: get_permission_keys
* **Signature:** `unsigned long long sys_get_perm_key(arg* a);`[cite: 1]
* **Description:** Signs and returns administrator (0) or user (1) permission keys tied to the calling process ID[cite: 1].

---

### Syscall 10: graduate
* **Signature:** `unsigned long long sys_graduate(arg* a);`[cite: 1]
* **Description:** Triggers the kernel process privilege escalation or graduation sequence[cite: 1].

---

### Syscall 11: draw_rect
* **Signature:** `unsigned long long sys_draw_rect(arg* a);`[cite: 1]
* **Description:** Renders a colored rectangle onto the framebuffer screen using coordinate arguments[cite: 1].

---

### Syscall 12: syscall_exit_handler
* **Signature:** `unsigned long long sys_exit_handler(arg* a);`[cite: 1]
* **Description:** Handles application exit flows utilizing stack pointer configurations and exit status codes[cite: 1].

---

### Syscall 13: ipc_recv
* **Signature:** `unsigned long long sys_ipc_recv(arg* a);`[cite: 1]
* **Description:** Blocks execution to receive inter-process communication messages[cite: 1].

---

### Syscall 14: ipc_send
* **Signature:** `unsigned long long sys_ipc_send(arg* a);`[cite: 1]
* **Description:** Transmits an inter-process communication message to a designated recipient[cite: 1].

---

### Syscall 15: getpid
* **Signature:** `unsigned long long sys_getpid(arg* a);`[cite: 1]
* **Description:** Queries and returns the process identifier of the current thread[cite: 1].

---

### Syscall 16: terminate
* **Signature:** `unsigned long long sys_terminate(arg* a);`[cite: 1]
* **Description:** Terminates a task or process based on passed pointers and parameters[cite: 1].

---

### Syscall 17: vfs_fstat
* **Signature:** `unsigned long long sys_fstat(arg* a);`[cite: 1]
* **Description:** Retrieves file status and metadata attributes for an open file descriptor[cite: 1].

---

### Syscall 18: read_stdin_scancodes
* **Signature:** `unsigned long long sys_read_stdin(arg* a);`[cite: 1]
* **Description:** Low-level keyboard input mechanism that enables interrupts and polls hardware scan codes into a buffer[cite: 1].

---

### Syscall 19: spawn
* **Signature:** `unsigned long long sys_spawn(arg* a);`[cite: 1]
* **Description:** Spawns a new child process from an executable binary path with arguments and configurations[cite: 1].

---

### Syscall 20: waitpid
* **Signature:** `unsigned long long sys_waitpid(arg* a);`[cite: 1]
* **Description:** Suspends caller execution until a specific target child process exits[cite: 1].

---

### Syscall 21: draw_image
* **Signature:** `unsigned long long sys_draw_image(arg* a);`[cite: 1]
* **Description:** Blits a raw pixel image buffer to specific screen coordinates and dimensions[cite: 1].

---

### Syscall 22: vmm_mmap
* **Signature:** `unsigned long long sys_mmap(arg* a);`[cite: 1]
* **Description:** Allocates or maps virtual memory pages for memory management[cite: 1].

---

### Syscall 23: vmm_munmap
* **Signature:** `unsigned long long sys_munmap(arg* a);`[cite: 1]
* **Description:** Unmaps designated virtual memory regions from process space[cite: 1].

---

### Syscall 24: set_signal_handler
* **Signature:** `unsigned long long sys_set_signal_handler(arg* a);`[cite: 1]
* **Description:** Registers a user-defined signal handler routine for a given signal type[cite: 1].

---

### Syscall 25: send_signal
* **Signature:** `unsigned long long sys_send_signal(arg* a);`[cite: 1]
* **Description:** Dispatches an asynchronous signal to a targeted process[cite: 1].

---

### Syscall 26: read_mouse
* **Signature:** `unsigned long long sys_read_mouse(arg* a);`[cite: 1]
* **Description:** Reads hardware mouse state packets into a designated user space buffer[cite: 1].

---

### Syscall 27: get_pixel
* **Signature:** `unsigned long long sys_get_pixel(arg* a);`[cite: 1]
* **Description:** Queries pixel color channels (RGB pointers) at exact screen coordinates[cite: 1].

---

### Syscall 28: ipc_send_nonblock
* **Signature:** `unsigned long long sys_ipc_send_nonblock(arg* a);`[cite: 1]
* **Description:** Performs a non-blocking IPC send operation[cite: 1].

---

### Syscall 29: ipc_recv_nonblock
* **Signature:** `unsigned long long sys_ipc_recv_nonblock(arg* a);`[cite: 1]
* **Description:** Performs a non-blocking IPC receive operation[cite: 1].

---

### Syscall 30: socket
* **Signature:** `unsigned long long sys_socket(arg* a);`[cite: 1]
* **Description:** Allocates and initializes a network socket resource[cite: 1].

---

### Syscall 31: socket_connect
* **Signature:** `unsigned long long sys_socket_connect(arg* a);`[cite: 1]
* **Description:** Connects an active network socket handle to a remote endpoint configuration[cite: 1].

---

### Syscall 32: socket_recv
* **Signature:** `unsigned long long sys_socket_recv(arg* a);`[cite: 1]
* **Description:** Receives incoming packet payloads from a connected network socket[cite: 1].

---

### Syscall 33: socket_send
* **Signature:** `unsigned long long sys_socket_send(arg* a);`[cite: 1]
* **Description:** Transmits packet data over an established network socket[cite: 1].

---

### Syscall 34: socket_close
* **Signature:** `unsigned long long sys_socket_close(arg* a);`[cite: 1]
* **Description:** Closes and tears down an active network socket interface[cite: 1].

---

### Syscall 35: get_ticks
* **Signature:** `unsigned long long sys_get_ticks(arg* a);`[cite: 1]
* **Description:** Returns the scaled system uptime ticks count[cite: 1].

---

### Syscall 36: sleep_ms
* **Signature:** `unsigned long long sys_sleep_ms(arg* a);`[cite: 1]
* **Description:** Suspends thread execution for a specified duration in milliseconds[cite: 1].

---

### Syscall 37: get_launchd_pid
* **Signature:** `unsigned long long sys_get_launchd_pid(arg* a);`[cite: 1]
* **Description:** Fetches the process identifier of the system initialization daemon (`launchd`)[cite: 1].

---

### Syscall 38: uname
* **Signature:** `unsigned long long sys_uname(arg* a);`[cite: 1]
* **Description:** Populates a `utsname` structure with system information like machine architecture, nodename, and kernel release strings[cite: 1].

---

### Syscall 39: sethostname
* **Signature:** `unsigned long long sys_sethostname(arg* a);`[cite: 1]
* **Description:** Sets the network node hostname string[cite: 1].

---

### Syscall 40: gethostname
* **Signature:** `unsigned long long sys_gethostname(arg* a);`[cite: 1]
* **Description:** Copies the current system host nodename into a user buffer[cite: 1].

---

### Syscall 41: rtc_get_time
* **Signature:** `unsigned long long sys_rtc_get_time(arg* a);`[cite: 1]
* **Description:** Retrieves the current real-time clock time into a `timespec` structure[cite: 1].

---

### Syscall 42: vfs_listdir
* **Signature:** `unsigned long long sys_listdir(arg* a);`[cite: 1]
* **Description:** Populates a buffer array with contents found inside a directory path[cite: 1].

---

### Syscall 43: uacpi_reboot
* **Signature:** `unsigned long long sys_reboot(arg* a);`[cite: 1]
* **Description:** Triggers a system reboot using the UACPI subsystem[cite: 1].

---

### Syscall 44: poweroff
* **Signature:** `unsigned long long sys_poweroff(arg* a);`[cite: 1]
* **Description:** Puts the machine into a power-off state via ACPI sleep state S5[cite: 1].

---

### Syscall 45: vfs_delete_file (alias)
* **Signature:** `unsigned long long sys_delete_file_alias(arg* a);`[cite: 1]
* **Description:** Provides an alternative entry point function to delete files via VFS resolution[cite: 1].

---

### Syscall 46: ioctl
* **Signature:** `unsigned long long sys_ioctl(arg* a);`[cite: 1]
* **Description:** Executes device-specific control input/output operations[cite: 1].

---

### Syscall 47: hvfs_create
* **Signature:** `unsigned long long sys_hvfs_create(arg* a);`[cite: 1]
* **Description:** Creates a hypervisor virtual filesystem resource node[cite: 1].

---

### Syscall 48: hvfs_set_type
* **Signature:** `unsigned long long sys_hvfs_set_type(arg* a);`[cite: 1]
* **Description:** Configures hypervisor VFS node types while preventing function type injection[cite: 1].

---

### Syscall 49: hvfs_set
* **Signature:** `unsigned long long sys_hvfs_set(arg* a);`[cite: 1]
* **Description:** Sets data values within the hypervisor virtual filesystem[cite: 1].

---

### Syscall 50: hvfs_get
* **Signature:** `unsigned long long sys_hvfs_get(arg* a);`[cite: 1]
* **Description:** Retrieves data payloads from hypervisor virtual filesystem entries[cite: 1].

---

### Syscall 51: hvfs_get_type
* **Signature:** `unsigned long long sys_hvfs_get_type(arg* a);`[cite: 1]
* **Description:** Queries the type definition of an HVFS path element[cite: 1].

---

### Syscall 52: hvfs_remove
* **Signature:** `unsigned long long sys_hvfs_remove(arg* a);`[cite: 1]
* **Description:** Removes a node from the hypervisor virtual filesystem[cite: 1].

---

### Syscall 53: hvfs_listdir
* **Signature:** `unsigned long long sys_hvfs_listdir(arg* a);`[cite: 1]
* **Description:** Lists directory items contained within an HVFS path branch[cite: 1].

---

### Syscall 54: hvfs_stat
* **Signature:** `unsigned long long sys_hvfs_stat(arg* a);`[cite: 1]
* **Description:** Fetches statistics and structural attributes of an HVFS path node[cite: 1].

---

### Syscall 55: chdir
* **Signature:** `unsigned long long sys_chdir(arg* a);`[cite: 1]
* **Description:** Changes the current working directory path context for the calling process[cite: 1].

---

### Syscall 56: getcwd
* **Signature:** `unsigned long long sys_getcwd(arg* a);`[cite: 1]
* **Description:** Copies the current working directory string into a user buffer[cite: 1].

---

### Syscall 57: realpath
* **Signature:** `unsigned long long sys_realpath(arg* a);`[cite: 1]
* **Description:** Resolves relative or absolute path names into a canonical absolute format path string[cite: 1].

---

### Syscall 58: ps
* **Signature:** `unsigned long long sys_ps(arg *a);`[cite: 1]
* **Description:** Populates task process listing arrays (`utask`) for system monitoring[cite: 1].

---

### Syscall 59: TTY Clear
* **Signature:** `unsigned long long sys_tty_clear(arg *a);`[cite: 1]
* **Description:** Clears the active text user interface console display buffer[cite: 1].

---

### Syscall 60: TTY switch
* **Signature:** `unsigned long long sys_tty_switch(arg* a);`[cite: 1]
* **Description:** Switches active text console virtual terminals or views[cite: 1].

---

### Syscall 61: TTY pixel
* **Signature:** `unsigned long long sys_tty_draw_pixel(arg *a);`[cite: 1]
* **Description:** Renders a single custom-colored pixel on the TTY console graphics layout[cite: 1].

---

### Syscall 62: TTY img
* **Signature:** `unsigned long long sys_tty_draw_img(arg *a);`[cite: 1]
* **Description:** Renders a 32-bit color image buffer onto specified coordinates on the TTY display screen[cite: 1].