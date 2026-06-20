Screenshot of Libra Booting
![Libra Screenshot](libra.png)
# Libra
## About the Kernel
The kernel is powered by Limine. It is a higher-half kernel with a GDT and IDT. It also includes a VFS which uses CPIO newc as its main format. A round-robin preemptive scheduler. And uses APIC as its primary interrupt handler. It has a PMM and VMM.
## My Goals
I've started this project as a small kernel. It's a sequel to my previous kernels (unreleased).
It aims to be a stable kernel and efficient. Hence the name Libra, but this kernel is still quite experimental, and we are super far away from this goal. So, it's a pleasure if anyone would like to help with this project. 
## Getting Started
### Project Instructions
I've taken inspiration from other kernels and see that this section for devs is mostly unclear.
The actual low-level kernel assembly code for booting isn't there. This kernel boots directly to C using Limine.
If you are unfamiliar with Limine, I'd recommend for you to check it out before developing here.
This is a higher-half kernel. Limine has mapped the pages already, and I'd prefer that we edit and shall not **REPLACE** the memory map.
### Creating a branch
Before submitting your edits. Please create a branch using this format: user_name-YY-MM-DD-v#.#.#-summary_of_your_edit.
And then create a pull request. Within a small period of time it might be accepted by me or not.

### Small Notes

The user Zirconium is also me, just another account, my Git was misconfigured at the time.

### Instructions to build
1. Install the dependencies listed below:
    - A `x86_64-elf` bare metal toolchain
    - `QEMU` (At least system)
    - `make`
    - `nasm`
    - `xorriso`
    - `libncurses-dev`
2. Configure the build
    1. Run `make menuconfig` to configure the kernel
    2. Run `make genconfig` to generate the configuration header
3. Build and Run
    1. Run `make -j8`
    2. Run `make run`
### Cleaning
Run `make clean`
## Feature Status

| Feature | Status | Notes |
|----------|----------|----------|
| PMM | Complete | Bitmap allocator |
| VMM | Complete | Higher-half mapping |
| Scheduler | Complete | Round-robin preemptive |
| uACPI | Complete | Namespace and initialization working |
| SMP | Not Started | |
| Filesystem | Complete | VFS Implemented |
| Userspace | WIP | Crashes on load with a #DE fault |
| AHCI | Complete | |
| GPT | Complete | |
| PCI | Complete | |
| APIC | Complete | Fully working with core selection |
| PIT | Complete | Fully working at 100hz with a sleep function |
| HPET | Not Started | |
| Syscalls | WIP | Complete but userspace doesn't work yet |
| User Security | WIP | IPC and buffer safety not complete yet |
| Accelerated Graphics Card | Not Started | |
## Release Scheme

- bca / bc# → broken builds (unstable / may not boot) 15-20 iterations max
- nca / nc# → nightly builds (experimental features) 12-15 iterations max
- rca / rc# → release candidates (stabilizing), 7-12 iterations max
- release → stable version

## User Security
Each process must require a permission from syscall #9 before calling any other syscalls. Each syscall except Syscall #9 will need an extra argument besides its normal arguments that contains a pointer to a permission. Each permission structure is shaped like this:
```c
typedef struct {
    uint64_t key;
    int claimedlevel;
} permission;
```
Level 0 is Administrator, Level 1 is User. The key given to the process is cryptographically signed with the process's PID to prevent security issues. When a process requests a **Level 0** permission, the kernel has to send an IPC message to the launchd executable (always has PID 1 and is the only process that is launched with ***Level -1 (4,294,967,295)*** automatically) to ask the user for permission to give the process (signed or unsigned) admin permissions. The response will be sent back to the kernel. And if declined the kernel will return -EACCES but if accepted the kernel will fill the buffer given in rbx.arg[1] and return 0 in rax. In each syscall, it checks if the claimedlevel matches the signed key for the process. It resigns it and checks it against the key given in the permission, if it is an admin operation and they both match, the admin operation is executed successfully, if they don't, then -EACCES is returned.
## VFS
Our VFS supports these operations:
- mkdir
- rmdir
- open
- read
- write
Each one is very versatile and useful.
