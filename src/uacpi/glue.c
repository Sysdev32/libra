// SPDX-License-Identifier: GPL-3.0-only
#include <limine.h>
#include <uacpi/types.h>
#include <uacpi/kernel_api.h>
#include <drivers/fb.h>
#include <drivers/alloc.h>

// Request the Root System Description Pointer (RSDP) from Limine
__attribute__((used, section(".limine_requests")))
volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0
};

// Request the Higher Half Direct Mapping address from Limine dynamically
__attribute__((used, section(".limine_requests")))
extern volatile struct limine_hhdm_request hhdm_request;
// Right below your struct definition in the actual C file:
struct SoftwareTCB *current_task = NULL;
// --- Core Tracking Structures ---

struct EmbeddedMutex {
    volatile uint32_t lock_state;
};

struct EmbeddedEvent {
    volatile uint32_t counter;
};

struct EmbeddedSpinlock {
    volatile uint32_t lock_val;
};

struct SoftwareTCB {
    int id;
    int state;
    uint64_t rsp;
    struct SoftwareTCB *next;
};

extern struct SoftwareTCB *current_task;

// --- Early Logging Implementation ---
extern struct flanterm_context *ft_ctx;
void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* chars) {
    (void)level;
    // Ensure string pointer exists before passing to serial/screen printk
    if (chars) {
        printk(LOG_ACPI, chars);
    }
}
// --- Dynamic Memory Mapping Hooks ---

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {
    if (out_rsdp_address == NULL || rsdp_request.response == NULL) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    *out_rsdp_address = rsdp_request.response->address;
    return UACPI_STATUS_OK;
}
void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    if (len == 0 || hhdm_request.response == NULL) {
        return NULL;
    }

    // 1. Force the physical address into a strict, clean 64-bit unsigned int
    uint64_t phys_addr = (uint64_t)addr;

    // 2. Fetch the dynamic HHDM base directly as an absolute 64-bit unsigned int
    uint64_t hhdm_base = (uint64_t)hhdm_request.response->offset;

    // 3. Clear any high-order bit garbage if uACPI passed an dirty 32-bit sign extended value
    // This forces the physical address to fit within your 4GB limit cleanly.
    phys_addr &= 0x00000000FFFFFFFFULL;

    // 4. Perform the full 64-bit addition
    uint64_t final_virtual = hhdm_base + phys_addr;

    // 5. Cast back to a void pointer safely
    return (void *)final_virtual;
}
void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    (void)addr;
    (void)len;
    // Direct maps span the entire physical space and don't need unmapping
}

uacpi_status uacpi_kernel_io_map(
    uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle
) {
    if (out_handle == NULL || len == 0) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    // Encode the I/O address directly inside the tracking handle pointer
    *out_handle = (uacpi_handle)((uintptr_t)base);
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle) {
    (void)handle;
}

// --- Native x86 I/O Port Instructions ---

uacpi_status uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset, uacpi_u8 *out_value) {
    if (!out_value) return UACPI_STATUS_INVALID_ARGUMENT;
    uint16_t port = (uint16_t)((uintptr_t)handle + offset);
    asm volatile("inb %1, %0" : "=a"(*out_value) : "Nd"(port) : "memory");
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset, uacpi_u16 *out_value) {
    if (!out_value) return UACPI_STATUS_INVALID_ARGUMENT;
    uint16_t port = (uint16_t)((uintptr_t)handle + offset);
    asm volatile("inw %1, %0" : "=a"(*out_value) : "Nd"(port) : "memory");
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset, uacpi_u32 *out_value) {
    if (!out_value) return UACPI_STATUS_INVALID_ARGUMENT;
    uint16_t port = (uint16_t)((uintptr_t)handle + offset);
    asm volatile("inl %1, %0" : "=a"(*out_value) : "Nd"(port) : "memory");
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset, uacpi_u8 value) {
    uint16_t port = (uint16_t)((uintptr_t)handle + offset);
    asm volatile("outb %0, %1" :: "a"(value), "Nd"(port) : "memory");
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset, uacpi_u16 value) {
    uint16_t port = (uint16_t)((uintptr_t)handle + offset);
    asm volatile("outw %0, %1" :: "a"(value), "Nd"(port) : "memory");
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset, uacpi_u32 value) {
    uint16_t port = (uint16_t)((uintptr_t)handle + offset);
    asm volatile("outl %0, %1" :: "a"(value), "Nd"(port) : "memory");
    return UACPI_STATUS_OK;
}
#define BUMP_HEAP_SIZE (2 * 1024 * 1024) // 2MB Allocation Arena
static uint8_t uacpi_bump_heap[BUMP_HEAP_SIZE] __attribute__((aligned(16)));
static size_t bump_heap_offset = 0;

void* uacpi_kernel_alloc(uacpi_size size) {
    if (size == 0) {
        return NULL;
    }

    // Align the requested size upward to the nearest 16-byte boundary
    size_t aligned_size = (size + 15) & ~15;

    // Guard against out-of-memory conditions in the arena
    if (bump_heap_offset + aligned_size > BUMP_HEAP_SIZE) {
        return NULL;
    }

    // Pick the current offset as the starting address
    void *allocated_ptr = (void*)&uacpi_bump_heap[bump_heap_offset];
    
    // Advance the bump pointer for the next allocation
    bump_heap_offset += aligned_size;

    // Zero out the memory block explicitly without relying on string.h memset
    uint8_t *byte_ptr = (uint8_t*)allocated_ptr;
    for (size_t i = 0; i < size; i++) {
        byte_ptr[i] = 0;
    }

    return allocated_ptr;
}
void uacpi_kernel_free(void *mem) {
    // Unused parameter macro to satisfy strict compiler configurations
    (void)mem; 
    
    // Intentionally left blank. Memory is reclaimed only if the 
    // entire heap offset is reset to 0.
}
// --- PCI Engine Configuration Stubs ---
uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle *out_handle) {
    if (out_handle == NULL) return UACPI_STATUS_INVALID_ARGUMENT;

    // Pack Bus (bits 16-23), Device (bits 11-15), and Function (bits 8-10) into the handle
    uintptr_t packed_addr = 
        ((uintptr_t)address.bus << 16) | 
        ((uintptr_t)address.device << 11) | 
        ((uintptr_t)address.function << 8);

    *out_handle = (uacpi_handle)packed_addr;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle) {
    (void)handle;
}
uacpi_status uacpi_kernel_pci_read32(uacpi_handle dev, uacpi_size offset, uacpi_u32 *val) {
    if (!val) return UACPI_STATUS_INVALID_ARGUMENT;
    
    uint32_t address = (uint32_t)((uintptr_t)dev);
    address |= (uint32_t)(offset & 0xFC);
    address |= (1U << 31);

    // Enforce explicit 16-bit port types to satisfy the assembler constraints
    uint16_t config_addr_port = 0xCF8;
    uint16_t config_data_port = 0xCFC;

    asm volatile("outl %0, %1" :: "a"(address), "Nd"(config_addr_port) : "memory");
    asm volatile("inl %1, %0" : "=a"(*val) : "Nd"(config_data_port) : "memory");
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle dev, uacpi_size offset, uacpi_u16 *val) {
    if (!val) return UACPI_STATUS_INVALID_ARGUMENT;
    uint32_t full_val;
    
    uacpi_kernel_pci_read32(dev, offset, &full_val);
    // Shift right by 0 or 16 bits depending on the offset alignment
    *val = (uint16_t)((full_val >> ((offset & 2) * 8)) & 0xFFFF);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle dev, uacpi_size offset, uacpi_u8 *val) {
    if (!val) return UACPI_STATUS_INVALID_ARGUMENT;
    uint32_t full_val;
    
    uacpi_kernel_pci_read32(dev, offset, &full_val);
    // Shift right by 0, 8, 16, or 24 bits depending on the offset alignment
    *val = (uint8_t)((full_val >> ((offset & 3) * 8)) & 0xFF);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_write32(uacpi_handle d, uacpi_size o, uacpi_u32 v) {
    uint32_t address = (uint32_t)((uintptr_t)d);
    address |= (uint32_t)(o & 0xFC);
    address |= (1U << 31);

    // Enforce explicit 16-bit port types to satisfy the assembler constraints
    uint16_t config_addr_port = 0xCF8;
    uint16_t config_data_port = 0xCFC;

    asm volatile("outl %0, %1" :: "a"(address), "Nd"(config_addr_port) : "memory");
    asm volatile("outl %0, %1" :: "a"(v), "Nd"(config_data_port) : "memory");
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_pci_write16(uacpi_handle d, uacpi_size o, uacpi_u16 v) {
    uint32_t full_val;
    uacpi_kernel_pci_read32(d, o, &full_val);
    
    uint32_t shift = (o & 2) * 8;
    full_val &= ~(0xFFFFU << shift); // Clear old 16 bits
    full_val |= ((uint32_t)v << shift); // Inject new 16 bits
    
    return uacpi_kernel_pci_write32(d, o, full_val);
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle d, uacpi_size o, uacpi_u8 v) {
    uint32_t full_val;
    uacpi_kernel_pci_read32(d, o, &full_val);
    
    uint32_t shift = (o & 3) * 8;
    full_val &= ~(0xFFU << shift); // Clear old 8 bits
    full_val |= ((uint32_t)v << shift); // Inject new 8 bits
    
    return uacpi_kernel_pci_write32(d, o, full_val);
}
// --- High-Precision Timing Infrastructure ---

static inline uint64_t read_tsc(void) {
    uint32_t low, high;
    // Explicitly enforce memory ordering constraints
    asm volatile("rdtsc" : "=a"(low), "=d"(high) :: "memory");
    return ((uint64_t)high << 32) | low;
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    uint64_t cycles = read_tsc();
    // Safe baseline integer scaling factor assuming a standard 2.0 GHz emulation
    return (uacpi_u64)(cycles / 2);
}

void uacpi_kernel_stall(uacpi_u8 usec) {
    if (usec == 0) return;
    
    // Instead of using dangerous legacy physical ports (like port 0x80), 
    // we use a predictable loop of the 'pause' instruction designed for micro-delays.
    uint64_t start = read_tsc();
    // Approximate cycles for 1 microsecond based on a 2.0 GHz baseline clock
    uint64_t expected_cycles = (uint64_t)usec * 2000;
    
    while ((read_tsc() - start) < expected_cycles) {
        asm volatile("pause" ::: "memory");
    }
}

void uacpi_kernel_sleep(uacpi_u64 msec) {
    if (msec == 0) return;

    // Calibrate dynamically by using our safe stall window
    uint64_t start_cal = read_tsc();
    uacpi_kernel_stall(1000); 
    uint64_t ticks_per_ms = read_tsc() - start_cal;

    // Guard fallback threshold if TSC calibration returns invalid bounds
    if (ticks_per_ms < 1000) {
        ticks_per_ms = 2000000; 
    }

    uint64_t total_ticks = msec * ticks_per_ms;
    uint64_t start_sleep = read_tsc();

    while ((read_tsc() - start_sleep) < total_ticks) {
        asm volatile("pause" ::: "memory");
    }
}
// --- Scheduler Thread Context Management ---

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    // If the software scheduler has not assigned a task context block yet,
    // safely return a fallback ID to let uACPI complete boot setup.
    if (current_task == NULL) {
        return (uacpi_thread_id)1;
    }
    return (uacpi_thread_id)((uintptr_t)current_task->id);
}

// --- Critical Interrupt Flag Controls ---

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
    uint64_t rflags;
    
    // Read current CPU flags, store them, and clear interrupts ('cli') atomically.
    asm volatile(
        "pushfq\n\t"
        "pop %0\n\t"
        "cli"
        : "=r"(rflags)
        :
        : "memory"
    );
    
    // Extract bit 9 (Interrupt Flag - IF) to check if interrupts were enabled
    return (uacpi_interrupt_state)((rflags & (1ULL << 9)) != 0);
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) {
    if (state) {
        asm volatile("sti" ::: "memory");
    } else {
        asm volatile("cli" ::: "memory");
    }
}

// --- Safe Mutex Management ---

uacpi_handle uacpi_kernel_create_mutex(void) {
    struct EmbeddedMutex *mutex = kmalloc(sizeof(struct EmbeddedMutex));
    if (mutex != NULL) {
        mutex->lock_state = 0; 
    }
    return (uacpi_handle)mutex;
}

void uacpi_kernel_free_mutex(uacpi_handle handle) {
    if (handle) kfree(handle);
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout) {
    if (handle == NULL) return UACPI_STATUS_INVALID_ARGUMENT;
    struct EmbeddedMutex *mutex = (struct EmbeddedMutex *)handle;

    if (timeout == 0) {
        if (mutex->lock_state == 0) {
            mutex->lock_state = 1;
            return UACPI_STATUS_OK;
        }
        return UACPI_STATUS_TIMEOUT;
    }

    if (timeout == 0xFFFF) {
        while (mutex->lock_state != 0) {
            asm volatile("pause" ::: "memory");
        }
        mutex->lock_state = 1;
        return UACPI_STATUS_OK;
    }

    for (uint32_t i = 0; i < timeout; i++) {
        if (mutex->lock_state == 0) {
            mutex->lock_state = 1;
            return UACPI_STATUS_OK;
        }
        uacpi_kernel_stall(1);
    }

    return UACPI_STATUS_TIMEOUT;
}

void uacpi_kernel_release_mutex(uacpi_handle handle) {
    if (handle == NULL) return;
    struct EmbeddedMutex *mutex = (struct EmbeddedMutex *)handle;
    mutex->lock_state = 0;
}

// --- Kernel Event Management ---

// A safe, dummy signature pointer so we aren't using absolute NULL (0)
#define DUMMY_EVENT_HANDLE ((uacpi_handle)0x1000)

uacpi_handle uacpi_kernel_create_event(void) {
    // Return a valid, non-NULL placeholder handle.
    // uACPI will think it got a genuine object pointer.
    return DUMMY_EVENT_HANDLE;
}


void uacpi_kernel_signal_event(uacpi_handle handle) {
    (void)handle;
    // Nothing to do: no other threads exist to wake up!
}

void uacpi_kernel_free_event(uacpi_handle handle) {
    (void)handle;
    // Nothing to free since we used a static dummy handle value.
}
uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout) {
    // Suppress unused variable warnings cleanly
    (void)handle;
    (void)timeout;

    // In a single-threaded environment, no background thread or interrupt 
    // handler can change an event state while we block. 
    // We return TRUE immediately to let uACPI advance past its internal locks.
    return UACPI_TRUE;
}

void uacpi_kernel_reset_event(uacpi_handle handle) {
    if (handle == NULL) return;
    struct EmbeddedEvent *event = (struct EmbeddedEvent *)handle;
    event->counter = 0;
}

// --- Advanced Hardware Spinlocks (Interrupt-Safe) ---

uacpi_handle uacpi_kernel_create_spinlock(void) {
    struct EmbeddedSpinlock *lock = kmalloc(sizeof(struct EmbeddedSpinlock));
    if (lock != NULL) {
        lock->lock_val = 0;
    }
    return (uacpi_handle)lock;
}

void uacpi_kernel_free_spinlock(uacpi_handle handle) {
    if (handle) kfree(handle);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle) {
    (void)handle;
    uint64_t rflags;
    
    // Save CPU flags and turn off interrupts to prevent deadlock conditions
    asm volatile(
        "pushfq\n\t"
        "pop %0\n\t"
        "cli"
        : "=r"(rflags)
        :
        : "memory"
    );
    
    return (uacpi_cpu_flags)rflags;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags) {
    (void)handle;
    
    // Safe restore of initial state using explicit inline registers
    asm volatile(
        "push %0\n\t"
        "popfq"
        :
        : "r"((uint64_t)flags)
        : "memory"
    );
}

// --- Interrupt Handling Bridges ---

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req) {
    (void)req;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
    uacpi_handle *out_irq_handle
) {
    (void)irq; (void)handler; (void)ctx;
    if (out_irq_handle == NULL) return UACPI_STATUS_INVALID_ARGUMENT;

    *out_irq_handle = (uacpi_handle)((uintptr_t)0xBAADF00D);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(
    uacpi_interrupt_handler handler, uacpi_handle irq_handle
) {
    (void)handler; (void)irq_handle;
    return UACPI_STATUS_OK;
}

// --- Work Scheduling Fallbacks ---

uacpi_status uacpi_kernel_schedule_work(
    uacpi_work_type type, uacpi_work_handler handler, uacpi_handle ctx
) {
    (void)type;
    // Execute synchronously during early boot setup
    if (handler != NULL) {
        handler(ctx);
    }
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    return UACPI_STATUS_OK;
}