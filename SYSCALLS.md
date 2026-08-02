# Syscalls of Libra

### Syscall 0: sys_read
* **Signature:** `int read(int fd, void *buf, size_t count, uint64_t offset)`
* **Arguments:**
    * `a->arg[0]` (`int fd`): User-space file descriptor (1 and 2 return 0; fds > 2 map to internal index `fd - 2`).
    * `a->arg[1]` (`void *buf`): Buffer pointer to store read data.
    * `a->arg[2]` (`size_t count`): Number of bytes to read.
    * `a->arg[3]` (`uint64_t offset`): File offset position.
* **Description:** Reads data from an open descriptor. Checks `fd_is_mnt` tracking flags to route execution either to mount device handlers or `vfs_read`.

---

### Syscall 1: sys_write
* **Signature:** `int write(int fd, const void *buf, size_t count)`
* **Arguments:**
    * `a->arg[0]` (`int fd`): Target descriptor.
    * `a->arg[1]` (`const void *buf`): Data buffer.
    * `a->arg[2]` (`size_t count`): Data length in bytes.
* **Description:** Writes to standard streams (fds 1/2) char-by-char via serial and TTY drivers. Fds > 2 delegate to mounted drivers (`write`) or `vfs_write_file`.

---

### Syscall 2: sys_open
* **Signature:** `int open(const char *path, int flags, uint32_t mode)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): File path string.
    * `a->arg[1]` (`int flags`): Access flags.
    * `a->arg[2]` (`uint32_t mode`): File permission modes.
* **Description:** Resolves path via `resolve_vfs_path`. Routes `/dev` paths to device mounts or standard files to `vfs_open`. Returns allocated internal fd shifted by +2.

---

### Syscall 3: sys_mkdir
* **Signature:** `int mkdir(const char *path, uint32_t mode)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): Target directory path.
    * `a->arg[1]` (`uint32_t mode`): Creation mode flags.
* **Description:** Resolves relative or absolute paths to canonical form and invokes `vfs_mkdir`.

---

### Syscall 4: sys_rmdir
* **Signature:** `int rmdir(const char *path)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): Directory path to remove.
* **Description:** Resolves target path and invokes `vfs_rmdir`.

---

### Syscall 5: sys_close
* **Signature:** `int close(int fd)`
* **Arguments:**
    * `a->arg[0]` (`int fd`): Descriptor handle to close.
* **Description:** Frees allocated descriptor state in VFS (`vfs_free_fd`) and resets internal tracking flags for fds > 2.

---

### Syscall 6: sys_move_file
* **Signature:** `int move_file(int fd, const char *new_path)`
* **Arguments:**
    * `a->arg[0]` (`int fd`): Active file descriptor to relocate (`fd - 2`).
    * `a->arg[1]` (`const char *new_path`): Destination path string.
* **Description:** Resolves target destination path and invokes `vfs_move_file` on the target open file handle.

---

### Syscall 7: sys_create_file
* **Signature:** `int create_file(void *ctx, const char *path, uint32_t flags)`
* **Arguments:**
    * `a->arg[0]` (`void *ctx`): Context pointer passed to file creation routine.
    * `a->arg[1]` (`const char *path`): Target file path.
    * `a->arg[2]` (`uint32_t flags`): File creation flags.
* **Description:** Resolves `path` and creates a node via `create()` for `/dev` routes or `vfs_create_file()` for VFS routes. Returns handle offset by +2.

---

### Syscall 8: sys_delete_file
* **Signature:** `int delete_file(const char *path)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): File path string to un-link.
* **Description:** Resolves target file path and delegates to `vfs_delete_file`.

---

### Syscall 9: sys_get_perm_key
* **Signature:** `int get_perm_key(int type, permission *out_perm)`
* **Arguments:**
    * `a->arg[0]` (`int type`): Permission type selector (`0` for admin key, `1` for user key).
    * `a->arg[1]` (`permission *out_perm`): User-space pointer to `permission` struct.
* **Description:** Cryptographically signs administrative or user keys using caller PID (`sign_key_with_pid`) and copies resulting `permission` metadata back to caller.

---

### Syscall 10: sys_graduate
* **Signature:** `void graduate(void)`
* **Arguments:** None.
* **Description:** Invokes the kernel process privilege escalation/graduation sequence.

---

### Syscall 11: sys_draw_rect
* **Signature:** `void draw_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint8_t r, uint8_t g, uint8_t b)`
* **Arguments:**
    * `a->arg[0]` (`uint64_t x`): Screen horizontal start coordinate.
    * `a->arg[1]` (`uint64_t y`): Screen vertical start coordinate.
    * `a->arg[2]` (`uint64_t w`): Rectangle width.
    * `a->arg[3]` (`uint64_t h`): Rectangle height.
    * `a->arg[4]` (`uint8_t r`): Red color channel.
    * `a->arg[5]` (`uint8_t g`): Green color channel.
    * `a->arg[6]` (`uint8_t b`): Blue color channel.
* **Description:** Directly renders a colored rectangle onto the system framebuffer.

---

### Syscall 12: sys_exit_handler
* **Signature:** `void exit_handler(uint64_t status)`
* **Arguments:**
    * `a->arg[0]` (`uint64_t status`): Exit status code.
* **Description:** Captures active caller stack pointer (`get_rsp()`) and passes execution context to kernel task exit handler `syscall_exit_handler`.

---

### Syscall 13: sys_ipc_recv
* **Signature:** `void ipc_recv(void *buf, uint64_t size, uint32_t *sender_pid)`
* **Arguments:**
    * `a->arg[0]` (`void *buf`): User-space buffer to write incoming payload.
    * `a->arg[1]` (`uint64_t size`): Maximum byte length to receive.
    * `a->arg[2]` (`uint32_t *sender_pid`): Pointer to receive sender PID metadata.
* **Description:** Blocking inter-process communication call to receive incoming messages.

---

### Syscall 14: sys_ipc_send
* **Signature:** `void ipc_send(uint64_t target_pid, void *buf, uint64_t size)`
* **Arguments:**
    * `a->arg[0]` (`uint64_t unused`): Unused parameter index.
    * `a->arg[1]` (`uint64_t target_pid`): Target recipient PID.
    * `a->arg[2]` (`void *buf`): Payload buffer.
    * `a->arg[3]` (`uint64_t size`): Byte size of payload.
* **Description:** Transmits an IPC message buffer to a target process.

---

### Syscall 15: sys_getpid
* **Signature:** `uint64_t getpid(void)`
* **Arguments:** None.
* **Description:** Returns the process identifier of the active executing task.

---

### Syscall 16: sys_terminate
* **Signature:** `int terminate(void *proc_ptr, uint64_t pid)`
* **Arguments:**
    * `a->arg[0]` (`void *proc_ptr` / `uint64_t pid`): Target process handle/PID.
* **Description:** Terminates the target process or task handle.

---

### Syscall 17: sys_fstat
* **Signature:** `int fstat(int fd, struct vfs_stat *statbuf)`
* **Arguments:**
    * `a->arg[0]` (`int fd`): User file descriptor (`fd - 2`).
    * `a->arg[1]` (`struct vfs_stat *statbuf`): Output metadata buffer pointer.
* **Description:** Populates a VFS stat structure with file details. Fds <= 2 return `-EBADF`.

---

### Syscall 18: sys_read_stdin
* **Signature:** `uint64_t read_stdin_scancodes(uint8_t *buf, uint64_t count)`
* **Arguments:**
    * `a->arg[0]` (`uint8_t *buf`): Destination scan code buffer.
    * `a->arg[1]` (`uint64_t count`): Number of scan code bytes requested.
* **Description:** Enables interrupts (`sti`) and polls hardware scan code inputs until the requested number of bytes are buffered.

---

### Syscall 19: sys_spawn
* **Signature:** `int spawn(const char *path, uint64_t flags, char **argv, const char *cwd)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): Executable binary path.
    * `a->arg[1]` (`uint64_t flags`): Process spawn configuration parameters.
    * `a->arg[2]` (`char **argv`): Null-terminated array of argument strings.
    * `a->arg[3]` (`const char *cwd`): Initial working directory path.
* **Description:** Creates and schedules a new child process executing from a specified binary path.

---

### Syscall 20: sys_waitpid
* **Signature:** `int waitpid(uint64_t pid)`
* **Arguments:**
    * `a->arg[0]` (`uint64_t pid`): Target process identifier.
* **Description:** Blocks execution until target child PID finishes execution.

---

### Syscall 21: sys_draw_image
* **Signature:** `void draw_image(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint8_t *img_buf)`
* **Arguments:**
    * `a->arg[0]` (`uint64_t x`): Top-left destination X coordinate.
    * `a->arg[1]` (`uint64_t y`): Top-left destination Y coordinate.
    * `a->arg[2]` (`uint64_t w`): Image width.
    * `a->arg[3]` (`uint64_t h`): Image height.
    * `a->arg[4]` (`uint8_t *img_buf`): Pixel array buffer pointer.
* **Description:** Blits a raw image buffer onto the system framebuffer.

---

### Syscall 22: sys_mmap
* **Signature:** `void *mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset)`
* **Arguments:**
    * `a->arg[0]` (`void *addr`): Suggested virtual address.
    * `a->arg[1]` (`size_t length`): Region size in bytes.
    * `a->arg[2]` (`int prot`): Memory protection permissions.
    * `a->arg[3]` (`int flags`): Mapping type flags.
    * `a->arg[4]` (`int fd`): File descriptor source (if file-backed).
    * `a->arg[5]` (`int64_t offset`): Byte offset within file source.
* **Description:** Allocates or maps virtual memory pages into process address space via `vmm_mmap`.

---

### Syscall 23: sys_munmap
* **Signature:** `int munmap(void *addr, size_t length)`
* **Arguments:**
    * `a->arg[0]` (`void *addr`): Base virtual address.
    * `a->arg[1]` (`size_t length`): Length of region to unmap.
* **Description:** Unmaps virtual memory pages from process space via `vmm_munmap`.

---

### Syscall 24: sys_set_signal_handler
* **Signature:** `int set_signal_handler(int sig, uint64_t handler_addr)`
* **Arguments:**
    * `a->arg[0]` (`int sig`): Signal identifier.
    * `a->arg[1]` (`uint64_t handler_addr`): Function entry address for signal routine.
* **Description:** Binds a user signal handler callback to a specific signal number.

---

### Syscall 25: sys_send_signal
* **Signature:** `int send_signal(int target_pid, int sig)`
* **Arguments:**
    * `a->arg[0]` (`int target_pid`): Target process ID.
    * `a->arg[1]` (`int sig`): Signal number to deliver.
* **Description:** Dispatches an asynchronous signal event to target task.

---

### Syscall 26: sys_read_mouse
* **Signature:** `int read_mouse(void *buf, uint64_t count)`
* **Arguments:**
    * `a->arg[0]` (`void *buf`): Destination user-space packet buffer.
    * `a->arg[1]` (`uint64_t count`): Max size or event count to read.
* **Description:** Fills user buffer with hardware mouse input state packet data.

---

### Syscall 27: sys_get_pixel
* **Signature:** `void get_pixel(uint64_t x, uint64_t y, uint8_t *r, uint8_t *g, uint8_t *b)`
* **Arguments:**
    * `a->arg[0]` (`uint64_t x`): Screen X position.
    * `a->arg[1]` (`uint64_t y`): Screen Y position.
    * `a->arg[2]` (`uint8_t *r`): Output pointer for Red channel value.
    * `a->arg[3]` (`uint8_t *g`): Output pointer for Green channel value.
    * `a->arg[4]` (`uint8_t *b`): Output pointer for Blue channel value.
* **Description:** Reads active pixel color channels at given screen coordinates directly from framebuffer.

---

### Syscall 28: sys_ipc_send_nonblock
* **Signature:** `int ipc_send_nonblock(uint64_t target_pid, void *buf, uint64_t size)`
* **Arguments:**
    * `a->arg[0]` (`uint64_t target_pid`): Recipient PID.
    * `a->arg[1]` (`void *buf`): Message buffer.
    * `a->arg[2]` (`uint64_t size`): Message length.
* **Description:** Non-blocking IPC transmit variant; returns immediately if recipient queue is full.

---

### Syscall 29: sys_ipc_recv_nonblock
* **Signature:** `int ipc_recv_nonblock(void *buf, uint64_t size, uint64_t timeout)`
* **Arguments:**
    * `a->arg[0]` (`void *buf`): Incoming payload buffer.
    * `a->arg[1]` (`uint64_t size`): Buffer size limit.
    * `a->arg[2]` (`uint64_t timeout`): Non-blocking timeout parameter.
* **Description:** Non-blocking IPC receive operation.

---

### Syscall 30: sys_socket
* **Signature:** `int socket(uint64_t domain, uint64_t type)`
* **Arguments:**
    * `a->arg[0]` (`uint64_t domain`): Network protocol family.
    * `a->arg[1]` (`uint64_t type`): Socket semantics type.
* **Description:** Allocates an entry in the global network socket table (`sockets`). Returns array index handle.

---

### Syscall 31: sys_socket_connect
* **Signature:** `int socket_connect(int sock_fd, uint64_t addr_info)`
* **Arguments:**
    * `a->arg[0]` (`int sock_fd`): Socket index in system socket table.
    * `a->arg[1]` (`uint64_t addr_info`): Encoded remote IP/port address parameter.
* **Description:** Executes network socket connection routine (`sock.connect`).

---

### Syscall 32: sys_socket_recv
* **Signature:** `int socket_recv(int sock_fd, void *buf, uint64_t size)`
* **Arguments:**
    * `a->arg[0]` (`int sock_fd`): Target socket handle.
    * `a->arg[1]` (`void *buf`): Payload destination buffer.
    * `a->arg[2]` (`uint64_t size`): Max receive length.
* **Description:** Invokes socket driver receive routine (`sock.recv`).

---

### Syscall 33: sys_socket_send
* **Signature:** `int socket_send(int sock_fd, void *buf, uint64_t size)`
* **Arguments:**
    * `a->arg[0]` (`int sock_fd`): Active socket handle.
    * `a->arg[1]` (`void *buf`): Data buffer to send.
    * `a->arg[2]` (`uint64_t size`): Byte count.
* **Description:** Invokes socket driver transmit routine (`sock.send`).

---

### Syscall 34: sys_socket_close
* **Signature:** `int socket_close(int sock_fd)`
* **Arguments:**
    * `a->arg[0]` (`int sock_fd`): Target socket handle.
* **Description:** Tears down active network socket interface via `sock.close`.

---

### Syscall 35: sys_get_ticks
* **Signature:** `uint64_t get_ticks(void)`
* **Arguments:** None.
* **Description:** Returns total timer ticks scaled by a multiplier factor of 10 (`ticks * 10`).

---

### Syscall 36: sys_sleep_ms
* **Signature:** `void sleep_ms(uint64_t ms)`
* **Arguments:**
    * `a->arg[0]` (`uint64_t ms`): Sleep duration in milliseconds.
* **Description:** Suspends thread execution for specified time period.

---

### Syscall 37: sys_get_launchd_pid
* **Signature:** `uint64_t get_launchd_pid(void)`
* **Arguments:** None.
* **Description:** Queries and returns process ID of system initialization daemon (`launchd`).

---

### Syscall 38: sys_uname
* **Signature:** `int uname(struct utsname *buf)`
* **Arguments:**
    * `a->arg[0]` (`struct utsname *buf`): Target information structure pointer.
* **Description:** Populates caller `utsname` buffer with machine architecture, nodename, kernel version, and release metadata.

---

### Syscall 39: sys_sethostname
* **Signature:** `int sethostname(const char *name, size_t len)`
* **Arguments:**
    * `a->arg[0]` (`const char *name`): New host name buffer.
    * `a->arg[1]` (`size_t len`): Name length limit.
* **Description:** Updates global kernel network node hostname (`nodename`).

---

### Syscall 40: sys_gethostname
* **Signature:** `int gethostname(char *name, size_t len)`
* **Arguments:**
    * `a->arg[0]` (`char *name`): Target string buffer.
    * `a->arg[1]` (`size_t len`): Buffer size limit.
* **Description:** Copies active kernel `nodename` into user space buffer.

---

### Syscall 41: sys_rtc_get_time
* **Signature:** `void rtc_get_time(struct timespec *time)`
* **Arguments:**
    * `a->arg[0]` (`struct timespec *time`): Destination time structure pointer.
* **Description:** Queries real-time clock hardware and populates `timespec`.

---

### Syscall 42: sys_listdir
* **Signature:** `int listdir(const char *path, char **out_buf, uint64_t max_entries)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): Target directory path.
    * `a->arg[1]` (`char **out_buf`): Destination array pointer for entries.
    * `a->arg[2]` (`uint64_t max_entries`): Maximum entries count limit.
* **Description:** Resolves directory path and lists contained file entries using `vfs_listdir`.

---

### Syscall 43: sys_reboot
* **Signature:** `void reboot(void)`
* **Arguments:** None.
* **Description:** Invokes ACPI hardware reset handler (`uacpi_reboot`).

---

### Syscall 44: sys_poweroff
* **Signature:** `void poweroff(void)`
* **Arguments:** None.
* **Description:** Transitions machine into ACPI S5 shutdown power state (`uacpi_enter_sleep_state`).

---

### Syscall 45: sys_delete_file_alias
* **Signature:** `int delete_file_alias(const char *path)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): Target path string.
* **Description:** Alias function entry point for file deletion path resolution and un-linking (`vfs_delete_file`).

---

### Syscall 46: sys_ioctl
* **Signature:** `int ioctl(int fd, uint64_t cmd, void *arg)`
* **Arguments:**
    * `a->arg[0]` (`int fd`): Device or file descriptor.
    * `a->arg[1]` (`uint64_t cmd`): Device control request code.
    * `a->arg[2]` (`void *arg`): Control argument or buffer pointer.
* **Description:** Passes control requests directly to device driver interfaces via `ioctl`.

---

### Syscall 47: sys_hvfs_create
* **Signature:** `int hvfs_create(const char *path)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): Target HVFS path node string.
* **Description:** Creates a dynamic node entry inside the Hierarchical Variable and Function System.

---

### Syscall 48: sys_hvfs_set_type
* **Signature:** `int hvfs_set_type(const char *path, uint64_t type)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): Target HVFS path.
    * `a->arg[1]` (`uint64_t type`): HVFS node type enum value.
* **Description:** Configures node data type (explicitly blocks setting `HVFS_TYPE_FUNCTION` types from user space).

---

### Syscall 49: sys_hvfs_set
* **Signature:** `int hvfs_set(const char *path, const void *data, size_t size)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): Target HVFS path.
    * `a->arg[1]` (`const void *data`): Pointer to input buffer.
    * `a->arg[2]` (`size_t size`): Byte length of input payload.
* **Description:** Writes data payload to specified HVFS variable node.

---

### Syscall 50: sys_hvfs_get
* **Signature:** `int hvfs_get(const char *path, void *out_buf, size_t size)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): Target HVFS path.
    * `a->arg[1]` (`void *out_buf`): Destination buffer pointer.
    * `a->arg[2]` (`size_t size`): Output buffer capacity.
* **Description:** Reads data contents stored within an HVFS node.

---

### Syscall 51: sys_hvfs_get_type
* **Signature:** `int hvfs_get_type(const char *path, hvfs_type_t *out_type)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): Target HVFS path.
    * `a->arg[1]` (`hvfs_type_t *out_type`): Pointer to receive node type tag.
* **Description:** Queries data type classification of targeted HVFS node.

---

### Syscall 52: sys_hvfs_remove
* **Signature:** `int hvfs_remove(const char *path)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): HVFS node path to delete.
* **Description:** Removes a variable or path node from HVFS tree structure.

---

### Syscall 53: sys_hvfs_listdir
* **Signature:** `int hvfs_listdir(const char *path, char *out_buf, size_t max_size)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): HVFS directory node path.
    * `a->arg[1]` (`char *out_buf`): Buffer to store child list names.
    * `a->arg[2]` (`size_t max_size`): Output buffer size limit.
* **Description:** Lists child node entries present under an HVFS path branch.

---

### Syscall 54: sys_hvfs_stat
* **Signature:** `int hvfs_stat(const char *path, hvfs_stat_t *out_stat)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): Target HVFS path.
    * `a->arg[1]` (`hvfs_stat_t *out_stat`): Pointer to output stat structure.
* **Description:** Fetches node status and metadata statistics from HVFS tree.

---

### Syscall 55: sys_chdir
* **Signature:** `int chdir(const char *path)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): Target working directory path.
* **Description:** Canonicalizes path input and updates current working directory context for process execution.

---

### Syscall 56: sys_getcwd
* **Signature:** `int getcwd(char *buf, size_t size)`
* **Arguments:**
    * `a->arg[0]` (`char *buf`): Buffer pointer for path string.
    * `a->arg[1]` (`size_t size`): Buffer byte capacity.
* **Description:** Copies active working directory string into user buffer. Returns 0 on success or `-1` on error.

---

### Syscall 57: sys_realpath
* **Signature:** `char *realpath(const char *path, char *resolved_path)`
* **Arguments:**
    * `a->arg[0]` (`const char *path`): Absolute or relative input path.
    * `a->arg[1]` (`char *resolved_path`): Optional user destination buffer (allocates kernel memory if `NULL`).
* **Description:** Resolves absolute path string. Returns string memory address on success or `-ENOENT` on error.

---

### Syscall 58: sys_ps
* **Signature:** `int ps(struct utask *task_list, uint64_t max_tasks)`
* **Arguments:**
    * `a->arg[0]` (`struct utask *task_list`): Destination array pointer for task metadata.
    * `a->arg[1]` (`uint64_t max_tasks`): Maximum array elements.
* **Description:** Copies current system task/process list information into user buffer array.

---

### Syscall 59: sys_tty_clear
* **Signature:** `void tty_clear(void)`
* **Arguments:** None.
* **Description:** Logs debug trace string and clears active graphics TTY text console buffer.

---

### Syscall 60: sys_tty_switch
* **Signature:** `void tty_switch(uint64_t tty_index)`
* **Arguments:**
    * `a->arg[0]` (`uint64_t tty_index`): Target zero-indexed TTY terminal number.
* **Description:** Switches active foreground TTY terminal display context.

---

### Syscall 61: sys_tty_draw_pixel
* **Signature:** `int tty_draw_pixel(uint64_t x, uint64_t y, uint32_t color)`
* **Arguments:**
    * `a->arg[0]` (`uint64_t x`): Screen horizontal coordinate.
    * `a->arg[1]` (`uint64_t y`): Screen vertical coordinate.
    * `a->arg[2]` (`uint32_t color`): 32-bit ARGB/XRGB color payload.
* **Description:** Renders a single pixel directly onto active TTY display output.

---

### Syscall 62: sys_tty_draw_img
* **Signature:** `int tty_draw_img(int start_x, int start_y, unsigned int *img_buffer, int w, int h)`
* **Arguments:**
    * `a->arg[0]` (`int start_x`): Left screen offset.
    * `a->arg[1]` (`int start_y`): Top screen offset.
    * `a->arg[2]` (`unsigned int *img_buffer`): 32-bit flat pixel array buffer pointer.
    * `a->arg[3]` (`int w`): Image pixel width.
    * `a->arg[4]` (`int h`): Image pixel height.
* **Description:** Iterates through flat 1D image array and renders pixels to TTY display offset. Performs bounds check on `img_buffer`, `w`, and `h`.

---

### Syscall 63: sys_tty_draw_rect
* **Signature:** `int tty_draw_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint32_t color)`
* **Arguments:**
    * `a->arg[0]` (`uint64_t x`): Origin X position.
    * `a->arg[1]` (`uint64_t y`): Origin Y position.
    * `a->arg[2]` (`uint64_t w`): Width in pixels.
    * `a->arg[3]` (`uint64_t h`): Height in pixels.
    * `a->arg[4]` (`uint32_t color`): 32-bit color value.
* **Description:** Fills a rectangular region on active TTY display using `tty_draw_pixel`.