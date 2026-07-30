// SPDX-License-Identifier: GPL-3.0-only
#include <arch/x86_64/idt.h>
#include <string.h>
#include <errno.h>
#include <drivers/fb.h>
#include <drivers/alloc.h>
#include <limine.h>
#include <arch/x86_64/schedule.h>
#include <drivers/hvfs.h>
#include <fs/vfs.h>
#include <uacpi/uacpi.h>
#include <uacpi/sleep.h>
#include <stdint.h>
#include <drivers/net/HTTP.h>
#include <drivers/net/IPV4.h>
#include <drivers/net/TCP.h>
#include <drivers/net/UDP.h>
#include <fs/mnt.h>
#include <hals/net/RTL8139.h>
#include <systable.h>
// --- Freestanding String Helpers ---
#define PATH_MAX 512
size_t k_strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

void k_memcpy(void *dest, const void *src, size_t n) {
    char *d = (char *)dest;
    const char *s = (const char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

void k_strcpy(char *dest, const char *src) {
    while ((*dest++ = *src++));
}

// System stub: Replace this with your actual VFS directory check.
// Returns 1 if the path exists and is a directory, 0 otherwise.
__attribute__((weak)) int vfs_is_directory(const char *path) {
    (void)path;
    return 1; // Default stub assumes directory exists
}

// --- Path Canonicalizer (Handles '.', '..', and relative paths) ---

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
                k_strcpy(segments[depth], seg);
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

        size_t slen = k_strlen(segments[i]);
        if (pos + slen >= out_size) return -1;
        
        k_memcpy(out + pos, segments[i], slen);
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
        if (k_strlen(path) >= PATH_MAX) return -1;
        k_strcpy(combined, path);
    } else {
        // Relative path: prepend CWD
        size_t cwd_len = k_strlen(current_cwd);
        size_t path_len = k_strlen(path);

        if (cwd_len + 1 + path_len >= PATH_MAX) return -1;

        k_strcpy(combined, current_cwd);
        if (combined[cwd_len - 1] != '/') {
            combined[cwd_len] = '/';
            combined[cwd_len + 1] = '\0';
        }
        
        // Append relative path
        size_t end = k_strlen(combined);
        k_strcpy(combined + end, path);
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
    k_strcpy(current_cwd, canonical);
    return 0;
}

char *getcwd(char *buf, size_t size) {
    char* current_cwd = getpcwd();
    if (!buf) return 0;

    size_t len = k_strlen(current_cwd) + 1;
    if (size < len) return 0; // Buffer too small

    k_memcpy(buf, current_cwd, len);
    return buf;
}

extern struct flanterm_context *ft_ctx;
extern uint64_t set_signal_handler(int sig, uint64_t handler);
extern int send_signal(int pid, int sig);
uint64_t admin_seed = 0xCAFEF00DD1CE1ULL; 
uint64_t user_seed = 0xCAFEF11DEADBEULL; 
void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Freestanding 32-bit unsigned random number generator.
// Must be initialized with a non-zero state.
static uint32_t random(uint64_t* state) {
    uint64_t old_state = *state;
    
    // Advance internal state using a Linear Congruential Generator (LCG)
    *state = old_state * 6364136223846793005ULL + 1442695040888963407ULL;
    
    // Calculate PCG-XSH-RR output transformation
    uint32_t xorshifted = (uint32_t)(((old_state >> 18u) ^ old_state) >> 27u);
    uint32_t rot = (uint32_t)(old_state >> 59u);
    
    // Return rotated value
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}
typedef void (*interrupt)(struct InterruptRegisters *regs);
uint64_t admin_key;
uint64_t user_key;
typedef struct {
    interrupt intr;
    int vector;
} handle;
static const char kbd_us_keymap[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', /* Backspace */
  '\t', /* Tab */
  'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', /* Enter */
    0,  /* 29   - Control */
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   
    0,  /* 42   - Left Shift */
 '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   
    0,  /* 54   - Right Shift */
  '*',
    0,  /* 56   - Alt */
  ' ',  /* Space bar */
    0,  /* 58   - Caps lock */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 59-68 - F1 to F10 keys */
    0,  /* 69   - Num lock */
    0,  /* 70   - Scroll Lock */
    0,  /* 71   - Home key */
    0,  /* 72   - Up Arrow */
    0,  /* 73   - Page Up */
  '-',
    0,  /* 75   - Left Arrow */
    0,
    0,  /* 77   - Right Arrow */
  '+',
    0,  /* 79   - End key */
    0,  /* 80   - Down Arrow */
    0,  /* 81   - Page Down */
    0,  /* 82   - Insert Key */
    0,  /* 83   - Delete Key */
    0, 0, 0,
    0,  /* 87   - F11 Key */
    0,  /* 88   - F12 Key */
    0,  /* All others undefined */
};
// --- GLOBAL VARIABLES AND CONFIGURATION TRACKING ---
handle handles[512] = {};
#define MAX_OVERRIDES 32

struct interrupt_override {
    uint8_t irq_source;
    uint32_t gsi;
    uint16_t flags;
};

struct interrupt_override isa_overrides[MAX_OVERRIDES];
int override_count = 0;
struct madt_local_apic apic[32];
int lapicint;
extern volatile struct limine_hhdm_request hhdm_request;
uintptr_t ioapic_virtual_base = 0;
uintptr_t lapic_virtual_base = 0;

#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1

extern void idt_load(struct IDTPtr *ptr);
extern void intr(void);
extern void *pmm_alloc_pages(int order);
extern uint64_t exception_vector_table[34]; // Expanded to hold all 34 elements (0-33)

static struct IDTEntry idt[256];
static struct IDTPtr   idt_ptr;

// Wait until the controller is ready for us to SEND a command (Bit 1 must be 0)
void ps2_wait_write(void) {
    while (inb(0x64) & 2);
}

// Wait until the controller has DATA for us to read (Bit 0 must be 1)
void ps2_wait_read(void) {
    while (!(inb(0x64) & 1));
}
static void ps2_enable_mouse(void) {
    // Enable PS/2 mouse port 2 and turn on data reporting.
    ps2_wait_write();
    outb(0x64, 0xA8); // Enable Port 2 (Mouse)

    ps2_wait_write();
    outb(0x64, 0xA9); // Test Port 2
    ps2_wait_read();
    if (inb(0x60) != 0x00) {
        return; // No mouse or no PS/2 port 2 support.
    }

    // Enable mouse interrupts in the controller configuration byte.
    ps2_wait_write();
    outb(0x64, 0x20);
    ps2_wait_read();
    uint8_t config = inb(0x60);
    config |= (1 << 1); // Enable IRQ12 for port 2

    ps2_wait_write();
    outb(0x64, 0x60);
    ps2_wait_write();
    outb(0x60, config);

    // Select default mouse packet mode.
    ps2_wait_write();
    outb(0x64, 0xD4);
    ps2_wait_write();
    outb(0x60, 0xF6);
    ps2_wait_read();
    if (inb(0x60) != 0xFA) {
        return;
    }

    // Enable mouse streaming / data reporting.
    ps2_wait_write();
    outb(0x64, 0xD4);
    ps2_wait_write();
    outb(0x60, 0xF4);
    ps2_wait_read();
    if (inb(0x60) != 0xFA) {
        return;
    }
}

void ps2_init(void) {
    // 1. Disable both PS/2 channels (Port 1 and Port 2) so they don't flood us with data
    ps2_wait_write();
    outb(0x64, 0xAD); // Disable Port 1 (Keyboard)
    ps2_wait_write();
    outb(0x64, 0xA7); // Disable Port 2 (Mouse - okay if it doesn't exist)

    // 2. Flush the buffer (Read any leftover garbage data out of port 0x60)
    while (inb(0x64) & 1) {
        inb(0x60);
    }

    // 3. Read the Configuration Byte
    ps2_wait_write();
    outb(0x64, 0x20); // Command: Read Controller Configuration
    ps2_wait_read();
    uint8_t config = inb(0x60);

    // 4. Modify the Configuration Byte:
    // Clear Bit 0 (Disable Port 1 Interrupts)
    // Clear Bit 1 (Disable Port 2 Interrupts)
    // Clear Bit 6 (Disable Port 1 Translation - keeps scan codes predictable)
    config &= ~(1 << 0);
    config &= ~(1 << 1);
    config &= ~(1 << 6);

    // Write the modified Configuration Byte back
    ps2_wait_write();
    outb(0x64, 0x60); // Command: Write Controller Configuration
    ps2_wait_write();
    outb(0x60, config);

    // 5. Perform Controller Self-Test
    ps2_wait_write();
    outb(0x64, 0xAA); // Command: Test Controller
    ps2_wait_read();
    if (inb(0x60) != 0x55) {
        // Controller is broken or missing!
        return;
    }

    // 6. Perform Interface Test (Port 1)
    ps2_wait_write();
    outb(0x64, 0xAB); // Command: Test Port 1
    ps2_wait_read();
    if (inb(0x60) != 0x00) {
        // Keyboard interface test failed
        return;
    }

    // 7. Enable the Keyboard Port and Turn on Interrupts
    ps2_wait_write();
    outb(0x64, 0xAE); // Command: Enable Port 1

    // Re-read configuration byte to flip the interrupt bit back on
    ps2_wait_write();
    outb(0x64, 0x20);
    ps2_wait_read();
    config = inb(0x60);

    config |= (1 << 0); // Set Bit 0: Enable Port 1 Interrupt (IRQ 1)

    ps2_wait_write();
    outb(0x64, 0x60);
    ps2_wait_write();
    outb(0x60, config);

    // 8. Reset the actual Keyboard hardware device
    ps2_wait_write();
    outb(0x60, 0xFF); // Device Command: Reset
    ps2_wait_read();
    if (inb(0x60) == 0xFA) { // ACK (Acknowledge)
        ps2_wait_read();
        uint8_t self_test_res = inb(0x60); // Should be 0xAA for passed
        (void)self_test_res;
    }

    // 9. Enable the PS/2 mouse hardware on port 2.
    ps2_enable_mouse();
}
// --- CORE IOAPIC HARDWARE WINDOW INTERFACE ---

// 1. Core hardware write function using the index/data window
void ioapic_write(uintptr_t base, uint8_t reg_index, uint32_t value) {
    volatile uint32_t *regsel = (volatile uint32_t*)(base + 0x00);
    volatile uint32_t *iowin  = (volatile uint32_t*)(base + 0x10);

    *regsel = reg_index;
    *iowin = value;
}

// 2. Core hardware read function using the index/data window
uint32_t ioapic_read(uintptr_t base, uint8_t reg_index) {
    volatile uint32_t *regsel = (volatile uint32_t*)(base + 0x00);
    volatile uint32_t *iowin  = (volatile uint32_t*)(base + 0x10);

    *regsel = reg_index;
    return *iowin;
}

// 3. Redirection Table Entry setter function to map pins to IDT vectors
void ioapic_set_entry(uintptr_t base, uint8_t pin, uint8_t idt_vector, uint8_t target_apic_id) {
    uint8_t reg_low = 0x10 + (pin * 2);
    uint8_t reg_high = reg_low + 1;

    // High 32 bits: Put target local APIC ID in bits 24-31
    // Fixed to target direct physical ID to prevent multi-core execution stalls
    uint32_t value_high = (uint32_t)target_apic_id << 24;

    // Low 32 bits: Start with IDT Vector number (bits 0-7)
    // Bit 16 is 0 (unmasked/enabled)
    // Delivery mode is 000 (fixed)
    // Trigger mode is 0 (edge-triggered for ISA)
    uint32_t value_low = idt_vector;

    ioapic_write(base, reg_high, value_high);
    ioapic_write(base, reg_low, value_low);
}

// 4. Call this function when you parse Entry Type 1 in your MADT loop
void ioapic_init(uint32_t physical_address) {
    // Calculate virtual address using Limine's Higher-Half Direct Map offset
    ioapic_virtual_base = (uintptr_t)physical_address + hhdm_request.response->offset;
    
    // Read the version register to verify communication
    uint32_t version_reg = ioapic_read(ioapic_virtual_base, 0x01);
    int max_entries = ((version_reg >> 16) & 0xFF) + 1;

    // Mask (disable) all redirection entries by default for safety
    for (int i = 0; i < max_entries; i++) {
        uint8_t reg_low = 0x10 + (i * 2);
        uint8_t reg_high = reg_low + 1;

        // Bit 16 = 1 masks the interrupt line
        ioapic_write(ioapic_virtual_base, reg_low, 0x00010000);
        ioapic_write(ioapic_virtual_base, reg_high, 0x00000000);
    }
    
}

void lapic_init(uint32_t physical_address) {
    lapic_virtual_base = (uintptr_t)physical_address + hhdm_request.response->offset;
}

// --- ACPI MULTIPLE APIC DESCRIPTION TABLE (MADT) ENGINE ---

void parse_madt(struct acpi_table_madt *madt) {
    if (madt == NULL) return;

    printk(LOG_TRACE, "Parsing MADT. Local APIC Address: 0x%x\n", madt->local_apic_address);
    lapic_init(madt->local_apic_address);
    
    // 1. Find where the variable-length records begin
    // Skip the main header to land exactly at offset 0x2C
    uintptr_t current_addr = (uintptr_t)madt + sizeof(struct acpi_table_madt);
    
    // 2. Calculate exactly where the records end using the table's total length
    uintptr_t end_addr = (uintptr_t)madt + madt->header.length;
    for (int i = 0; i < 32; i++) {
        isa_overrides[i].irq_source = i;
        isa_overrides[i].gsi = i;
    }

    // 3. Loop through the records safely
    while (current_addr < end_addr) {
        struct madt_record_header *record = (struct madt_record_header *)current_addr;

        // Malformed table safety check: length cannot be 0 or push us past the end
        if (record->length == 0 || (current_addr + record->length) > end_addr) {
            printk(LOG_ERROR, "MADT parsing error: invalid record length %d\n", record->length);
            break;
        }

        // 4. Check the entry type and cast it to your specific structure
        switch (record->type) {
            case 0: {
                struct madt_local_apic *lapic = (struct madt_local_apic *)record;
                if (lapic->flags & 1) { // Bit 0 = Processor Enabled
                    apic[lapicint++] = *lapic;
                }   
                break;
            }
            case 1: {
                struct madt_io_apic *ioapic = (struct madt_io_apic *)record;
                
                ioapic_init(ioapic->io_apic_address);
                break;
            }
            case 2: {
                struct madt_interrupt_override *override = (struct madt_interrupt_override *)record;
                isa_overrides[override->irq_source].gsi = override->gsi;
                break;
            }
            case 5: {
                struct madt_local_apic_override *lapic_64 = (struct madt_local_apic_override *)record;
                printk(LOG_TRACE, "64-bit Local APIC Address Override: 0x%llx\n", lapic_64->local_apic_address_64);
                break;
            }
            default:
                // Types we don't care about right now (like NMIs) are skipped safely
                break;
        }

        // 5. Jump directly to the next record header using its precise length
        current_addr += record->length;
    }
}

// --- HARDWARE INTERACTION PORT WRAPPERS ---


void pic_disable(void) {
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);  
}

// --- IDT MANIPULATION DESCRIPTOR SUITE ---

static void idt_set_descriptor(uint8_t vector, void *isr, uint8_t attributes) {
    uint64_t addr = (uint64_t)isr;
    idt[vector].isr_low    = (uint16_t)(addr & 0xFFFF);
    idt[vector].kernel_cs  = 0x08; 
    idt[vector].ist        = 0;
    idt[vector].attributes = attributes;
    idt[vector].isr_mid    = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[vector].isr_high   = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[vector].reserved   = 0;
}

char* crop(char *dest, const char *src, int until) {
    for (int i = 0; i < until-1; i++) {
        dest[i] = src[i];
    }
    dest[until-1] = '\0';
    return dest; // Returns the pointer to the destination buffer
}
extern void exception_stub_44(void);
extern void exception_stub_33(void);
char nodename[65] = {0};
void idt_init(void) {
    memset(idt, 0, sizeof(struct IDTEntry) * 256);
    user_key = random(&user_seed);
    admin_key = random(&admin_seed);
    // 1. Loop exactly 32 times to map exception_vector_table[0] through [31]
    for (int i = 0; i < 32; i++) {
        idt_set_descriptor(i, (void*)exception_vector_table[i], 0x8E);
    }

    // Ensure Double Fault (#DF, vector 8) uses IST entry 1 to guarantee
    // a dedicated emergency stack (prevents triple-faults if the normal
    // kernel stack is corrupted).
    idt[8].ist = 1;

    // 2. Safely map vector 0x20 and 0x21 using slots 32 and 33 from the assembly landing pads
    idt_set_descriptor(0x20, (void*)exception_vector_table[32], 0x8E);
    idt_set_descriptor(0x21, (void*)exception_stub_33, 0xEE);
    idt_set_descriptor(0x2C, (void*)exception_stub_44, 0xEE);
    idt_set_descriptor(0x80, intr, 0xEE); // User Mode System Calls Gate

    // 3. Load the IDT pointer into the processor
    idt_ptr.limit = (sizeof(struct IDTEntry) * 256) - 1;
    idt_ptr.base  = (uint64_t)&idt;

    idt_load(&idt_ptr);

    // Debug: read back IDTR to ensure it was loaded correctly
    struct IDTPtr cur_idt;
    asm volatile("sidt %0" : "=m"(cur_idt));
    printk(LOG_DEBUG, "IDTR after lidt -> base=%p limit=0x%x\n", (void*)cur_idt.base, cur_idt.limit);

    // 4. Disable legacy PIC
    pic_disable();
    ps2_init();
    strcpy(nodename, "machine");
}

void lapic_eoi(void) {
    if (lapic_virtual_base == 0) return;
    
    volatile uint32_t *eoi_reg = (volatile uint32_t*)(lapic_virtual_base + 0xB0);
    *eoi_reg = 0;
}

void timer() {
    printk(LOG_TRACE, "huh");
    lapic_eoi();
}

void ioapic(struct acpi_table_madt* madt) {
    // 1. Disable the old 8259 PIC completely
    pic_disable();
    
    // 2. Parse MADT to find lapic_virtual_base and ioapic_virtual_base
    parse_madt(madt);
    
    // 3. Turn on the local APIC software enable flag
    if (lapic_virtual_base != 0) {
        volatile uint32_t *spurious_reg = (volatile uint32_t*)(lapic_virtual_base + 0xF0);
        // (1 << 8) sets the Software Enable bit to 1
        // 0xFF maps the spurious vector to index 255 in the IDT
        *spurious_reg = 0xFF | (1 << 8); 
    }

    // 4. Route ISA IRQs safely to IDT vectors via the IOAPIC targeting Core 0
    ioapic_set_entry(ioapic_virtual_base, 2, 0x20, 0x00);                       // PIT route mapping
    ioapic_set_entry(ioapic_virtual_base, isa_overrides[1].gsi, 0x21, 0);        // Keyboard IRQ1
    ioapic_set_entry(ioapic_virtual_base, isa_overrides[12].gsi, 0x2C, 0);       // Mouse IRQ12

}
typedef struct {
    uint64_t key;
    int claimedlevel;
} permission;
// Signs a data key combined with a specific process identifier (PID)
void sign_key_with_pid(const uint8_t* key, uint32_t key_len, uint32_t pid, uint8_t out_signature[32]) {
    uint8_t k_ipad[64] = {0};
    uint8_t k_opad[64] = {0};
    
    // Handle keys larger than block size (64 bytes)
    uint8_t prepared_key[64] = {0};
    if (key_len > 64) {
        sha256_hash(key, key_len, prepared_key);
    } else {
        for (uint32_t i = 0; i < key_len; i++) prepared_key[i] = key[i];
    }

    // XOR key with inner and outer padding constants
    for (int i = 0; i < 64; i++) {
        k_ipad[i] = prepared_key[i] ^ 0x36;
        k_opad[i] = prepared_key[i] ^ 0x5C;
    }

    // Combine inner padding with the 4-byte PID payload
    uint8_t inner_buffer[64 + 4];
    for (int i = 0; i < 64; i++) inner_buffer[i] = k_ipad[i];
    inner_buffer[64] = (pid >> 24) & 0xFF;
    inner_buffer[65] = (pid >> 16) & 0xFF;
    inner_buffer[66] = (pid >> 8) & 0xFF;
    inner_buffer[67] = pid & 0xFF;

    // First hash pass
    uint8_t inner_hash[32];
    sha256_hash(inner_buffer, 68, inner_hash);

    // Combine outer padding with inner hash result
    uint8_t outer_buffer[64 + 32];
    for (int i = 0; i < 64; i++) outer_buffer[i] = k_opad[i];
    for (int i = 0; i < 32; i++) outer_buffer[64 + i] = inner_hash[i];

    // Second hash pass to yield the final signature
    sha256_hash(outer_buffer, 96, out_signature);
}
uint64_t get_rsp(void) {
    uint64_t rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}
uint64_t signature_to_uint64_direct(const uint8_t* signature) {
    uint64_t result;
    // Copies the first 8 bytes of the signature directly into the uint64_t
    // Works safely across alignment boundaries
    for (int i = 0; i < 8; i++) {
        ((uint8_t*)&result)[i] = signature[i];
    }
    return result;
}


// ==========================================
// HIGH-LEVEL HTTP WRAPPERS
// ==========================================

// ==========================================
// SYSTEM SOCKET MULTIPLEXER
// ==========================================


volatile uint64_t ticks = 0;

volatile int last_scancode = -1;
char kgetc() {
    // Wait for ISR to register a new press
    while (last_scancode == -1 && (!(last_scancode & 0x80))) {
        __asm__ volatile("hlt"); // Save CPU power while waiting
    }

    uint8_t code = last_scancode;
    last_scancode = -1; // Clear the latch

    return kbd_us_keymap[last_scancode];
}
#include <stdbool.h>

// Global array tracking whether an allocated descriptor belongs to the custom mount framework
// Index maps to (system_fd - 2) slots

static void handle_syscall(struct InterruptRegisters *regs) {
    arg *args = (arg*)regs->rbx;
    regs->rax = systable[regs->rax](args);
}
// --- COMMON HARDWARE AND LEGACY INTERRUPT DISPATCH ROUTER ---

#define KBD_BUFFER_SIZE 256
#define MOUSE_BUFFER_SIZE 128

typedef struct {
    uint8_t data[KBD_BUFFER_SIZE];
    volatile uint32_t head;  // Must be volatile
    volatile uint32_t tail;  // Must be volatile
} kbd_buffer_t;

typedef struct {
    int8_t dx;
    int8_t dy;
    uint8_t buttons;
    uint8_t reserved;
} mouse_event_t;

typedef struct {
    mouse_event_t data[MOUSE_BUFFER_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
} mouse_buffer_t;

static volatile kbd_buffer_t kbd_buf = { .head = 0, .tail = 0 };
static volatile mouse_buffer_t mouse_buf = { .head = 0, .tail = 0 };
static uint8_t mouse_packet[3];
static int mouse_phase = 0;

// Call this inside your keyboard interrupt handler to save a scancode
void kbd_buffer_push(uint8_t scancode) {
    uint32_t next = (kbd_buf.head + 1) % KBD_BUFFER_SIZE;
    
    // If the buffer is full, discard the scancode to protect kernel memory
    if (next == kbd_buf.tail) {
        return;
    }
    
    kbd_buf.data[kbd_buf.head] = scancode;
    kbd_buf.head = next;
}

// This function checks if user space has anything to read
int kbd_buffer_pop(uint8_t *out_scancode) {
    if (kbd_buf.head == kbd_buf.tail) {
        return 0; // Buffer is empty
    }
    
    *out_scancode = kbd_buf.data[kbd_buf.tail];
    kbd_buf.tail = (kbd_buf.tail + 1) % KBD_BUFFER_SIZE;
    return 1; // Successfully popped a scancode
}

void mouse_buffer_push(mouse_event_t event) {
    uint32_t next = (mouse_buf.head + 1) % MOUSE_BUFFER_SIZE;
    if (next == mouse_buf.tail) {
        return; // Buffer full, drop oldest mouse event to preserve kernel stability
    }
    mouse_buf.data[mouse_buf.head] = event;
    mouse_buf.head = next;
}

int mouse_buffer_pop(mouse_event_t *out_event) {
    if (mouse_buf.head == mouse_buf.tail) {
        return 0;
    }
    *out_event = mouse_buf.data[mouse_buf.tail];
    mouse_buf.tail = (mouse_buf.tail + 1) % MOUSE_BUFFER_SIZE;
    return 1;
}
// System call wrapper exposed to your interrupt/syscall table
int sys_read_key(uint8_t *user_buffer) {
    uint8_t scancode;
    
    // Attempt to pull a key out of the queue
    if (kbd_buffer_pop(&scancode)) {
        // Safely write the byte to the user space pointer address
        *user_buffer = scancode;
        return 1; // Success
    }
    
    return 0; // No keys available right now
}

int read_mouse(void *user_buffer, uint64_t count) {
    if (user_buffer == NULL || count < sizeof(mouse_event_t)) {
        return -1;
    }

    mouse_event_t event;
    while (!mouse_buffer_pop(&event)) {
        asm volatile("sti; hlt" ::: "memory");
    }

    mouse_event_t *dest = (mouse_event_t*)user_buffer;
    *dest = event;
    return sizeof(mouse_event_t);
}

uint64_t intrhandler(struct InterruptRegisters* regs) {
    uint64_t vector = regs->int_no;

    // --- DISPATCH GATE A: SYSTEM CALL SYSTEM GATE (Vector 0x80 / 128) ---
    if (vector == 128 || vector == 0x80) {
        handle_syscall(regs);
        return 0;
    }

    // --- DISPATCH GATE B: SYSTEM PREEMPTIVE TIMER INTERRUPT (Vector 32) ---
    if (vector == 32) {
        ticks += 1;
        hid_keyboard_poll();
        tty_update_cursor();
        uint64_t new_rsp = schedule_preemptive((uint64_t)regs);
        lapic_eoi();
        return new_rsp;
    }
    if (vector == 33) {
    uint8_t scancode = inb(0x60);

    last_scancode = scancode;
    
    // Virtual Console Switching via Function Keys (F1 - F6)
    if (scancode == 0x3B) {
        tty_switch(0);
    } else if (scancode == 0x3C) {
        tty_switch(1);
    } else if (scancode == 0x3D) {
        tty_switch(2);
    } else if (scancode == 0x3E) {
        tty_switch(3);
    } else if (scancode == 0x3F) {
        tty_switch(4);
    } else if (scancode == 0x40) {
        tty_switch(5);
    }
    
    // Ensure it's a make code (press event)
    if (!(scancode & 0x80)) {
        char c = kbd_us_keymap[last_scancode];
        
        // Only forward standard printable character keys down to the TTY sub-system
        if (c >= ' ' && c <= '~') {
            tty_handle_input(c);
        }
    }
    
    lapic_eoi();
    return 0;
}
    if (vector == 44) {
        uint8_t byte = inb(0x60);

        if (mouse_phase == 0) {
            // Synchronize to the first byte: bit 3 must be set in a valid PS/2 packet.
            if (!(byte & 0x08)) {
                return 0;
            }
        }

        mouse_packet[mouse_phase++] = byte;
        if (mouse_phase < 3) {
            lapic_eoi();
            return 0;
        }

        mouse_phase = 0;
        mouse_event_t event = {
            .dx = (int8_t)mouse_packet[1],
            .dy = (int8_t)mouse_packet[2],
            .buttons = mouse_packet[0] & 0x07,
            .reserved = 0,
        };
        mouse_buffer_push(event);
        lapic_eoi();
        return 0;
    }
    for (int i = 0; i < 512; i++) {
        if ((uint64_t)handles[i].vector == vector && handles[i].intr != NULL) {
            // Call the registered device driver callback function
            handles[i].intr(regs);
            break;
        }
    }
    
    lapic_eoi();
    return 0;
}
// --- ARCHITECTURAL EXCEPTION NAMES STORAGE ENGINE ---

static const char *exception_names[] = {
    "Divide-by-Zero (#DE)",                  // 0
    "Debug (#DB)",                           // 1
    "Non-Maskable Interrupt (NMI)",         // 2
    "Breakpoint (#BP)",                     // 3
    "Overflow (#OF)",                        // 4
    "Bound Range Exceeded (#BR)",            // 5
    "Invalid Opcode (#UD)",                  // 6
    "Device Not Available (#NM)",            // 7
    "Double Fault (#DF)",                    // 8
    "Coprocessor Segment Overrun",           // 9
    "Invalid TSS (#TS)",                     // 10
    "Segment Not Present (#NP)",             // 11
    "Stack-Segment Fault (#SS)",             // 12
    "General Protection Fault (#GP)",        // 13
    "Page Fault (#PF)",                      // 14
    "Reserved / Unknown",                    // 15
    "x87 Floating-Point Exception (#MF)",    // 16
    "Alignment Check (#AC)",                 // 17
    "Machine Check (#MC)",                   // 18
    "SIMD Floating-Point Exception (#XM)",   // 19
    "Virtualization Exception (#VE)",        // 20
    "Control Protection Exception (#CP)",    // 21
    "Reserved", "Reserved", "Reserved",     // 22-24
    "Reserved", "Reserved", "Reserved",     // 25-27
    "Hypervisor Injection Exception (#HV)",  // 28
    "VMM Communication Exception (#VC)",     // 29
    "Security Exception (#SE)",              // 30
    "Reserved"                               // 31
};

// --- CORE CPU EXCEPTION DIAGNOSTICS AND CRASH DUMP ANALYSIS ENGINE ---
void sleep_ms(uint64_t ms)
{
    uint64_t tickst = ms / 10;

    uint64_t start = ticks;

    while ((ticks - start) < tickst) {

        __asm__ volatile("hlt"); // low power wait until next interrupt
    }
}

static const char* user_exceptions[] = {
    /* 0-7 */
    "Divide-By-Zero",              // 0
    "Debug Trap",                  // 1
    "Non-Maskable Interrupt",      // 2
    "Breakpoint",                  // 3
    "Overflow",                    // 4
    "Bound Range Fault",           // 5
    "Invalid Instruction",         // 6
    "Device Not Available",        // 7

    /* 8-15 */
    "Double Fault",               // 8 (kernel-level, usually not userspace)
    "Coprocessor Segment Overrun",// 9
    "Invalid TSS",                // 10
    "Segment Not Present",        // 11
    "Stack Fault",                // 12
    "Protection Fault",   // 13
    "Segmentation Fault",        // 14 (Page Fault)
    "Reserved Exception",         // 15

    /* 16-19 */
    "Floating Point Error",       // 16
    "Alignment Check Failure",    // 17
    "Machine Check Exception",    // 18
    "SIMD Floating Point Fault",  // 19

    /* 20-31 */
    "Reserved / Unknown",         // 20
    "Reserved / Unknown",         // 21
    "Reserved / Unknown",         // 22
    "Reserved / Unknown",         // 23
    "Reserved / Unknown",         // 24
    "Reserved / Unknown",         // 25
    "Reserved / Unknown",         // 26
    "Reserved / Unknown",         // 27
    "Reserved / Unknown",         // 28
    "Reserved / Unknown",         // 29
    "Reserved / Unknown",         // 30
    "Reserved / Unknown"          // 31
};
uint64_t exception_handler_c(struct InterruptRegisters *regs) {
    asm volatile ("sti");
    uint64_t vector = regs->int_no; 

    // --- CASE A: CRITICAL ARCHITECTURAL CPU EXCEPTIONS (0-31) ---
    if (vector < 32) {
        if (regs->cs != 0x1B) {
        char buffer[64];
        for (int i = 0; i < 64; i++) buffer[i] = 0; // Clear the buffer for safety
        if (vector > 255) {
            strcpy(buffer, "(invalid)");
        }
        
        printk(LOG_ERROR, "Vector Index    : %d (0x%x) %s\r\n", vector, vector, buffer);
        printk(LOG_ERROR, "Description     : %s\r\n", exception_names[vector]);
        printk(LOG_ERROR, "Error Code Mask : 0x%x\r\n", regs->error_code);
        printk(LOG_ERROR, "--------------------------------------------------\r\n");
        
        // Output code boundaries and stack tracking snapshots
        printk(LOG_ERROR, "Faulting Instruction Pointer (RIP): %p\r\n", regs->rip);
        printk(LOG_ERROR, "Faulting Stack Pointer       (RSP): %p\r\n", regs->rsp);
        printk(LOG_ERROR, "Code Segment Selector        (CS) : %x\r\n", regs->cs);
        printk(LOG_ERROR, "Processor Flag Mask      (RFLAGS) : %p\r\n", regs->rflags);
        
        // Specialize tracking read metrics for standard complex traps
        switch (vector) {
            case 13: { // General Protection Fault
                printk(LOG_ERROR, " -> Details: Memory segment selection or boundary access rule violation.\r\n");
                if (regs->error_code != 0) {
                    printk(LOG_ERROR, " -> Broken Selector Target Segment Index: GDT/LDT Slot %d\r\n", regs->error_code >> 3);
                }
                break;
            }
            case 14: { // Page Fault
                uint64_t faulting_address = 0;
                asm volatile("mov %%cr2, %0" : "=r"(faulting_address));
                printk(LOG_ERROR, " -> Details: Attempted to touch unmapped or protected virtual address layout space.\r\n");
                printk(LOG_ERROR, " -> Missing Destination Target (CR2): 0x%p\r\n", faulting_address);
                
                // Decode page fault reason flags out of error code bitmask
                printk(LOG_ERROR, " -> Fault Parameters: %s, %s, %s\r\n",
                    (regs->error_code & (1 << 0)) ? "Protection Violation" : "Non-Present Page",
                    (regs->error_code & (1 << 1)) ? "Write Operation" : "Read Operation",
                    (regs->error_code & (1 << 2)) ? "User Privilege Level" : "Supervisor Ring 0 Code"
                );
                break;
            }
            default:
                printk(LOG_ERROR, " -> Details: Fatal core execution anomaly, stalling CPU state pipeline layout.\r\n");
                break;
        }

        printk(LOG_ERROR, "--------------------------------------------------\r\n");
        printk(LOG_ERROR, "Register Dump:\r\n");
        printk(LOG_ERROR, "RAX: %p  RBX: %p  RCX: %p  RDX: %p\r\n", regs->rax, regs->rbx, regs->rcx, regs->rdx);
        printk(LOG_ERROR, "RSI: %p  RDI: %p  RBP: %p  R8 : %p\r\n", regs->rsi, regs->rdi, regs->rbp, regs->r8);
        printk(LOG_ERROR, "R9 : %p  R10: %p  R11: %p  R12: %p\r\n", regs->r9, regs->r10, regs->r11, regs->r12);
        printk(LOG_ERROR, "R13: %p  R14: %p  R15: %p\r\n", regs->r13, regs->r14, regs->r15);
        printk(LOG_ERROR, "==================================================\r\n");
        printk(LOG_INFO, "Rebooting in 5 seconds...\r\n");
        sleep_ms(1000);
        printk(LOG_INFO, "Rebooting in 4 seconds...\r\n");
        sleep_ms(1000);
        printk(LOG_INFO, "Rebooting in 3 seconds...\r\n");
        sleep_ms(1000);
        printk(LOG_INFO, "Rebooting in 2 seconds...\r\n");
        sleep_ms(1000);
        printk(LOG_INFO, "Rebooting in 1 seconds...\r\n");
        sleep_ms(1000);

        // Force definitive crash freeze so hardware doesn't pass broken state steps downward
        for (;;) {
            asm volatile("hlt");
        }
        } else {
            printk(LOG_NONE, "%s.\n", user_exceptions[vector]);
            return terminate(regs->rsp, getpid());
        }
    } 
    
    // --- CASE B: DYNAMIC INTERRUPT FALLBACK ROUTING PATH ---
    else {
        if (vector == 0x20) {
            uint64_t new_rsp = schedule_preemptive((uint64_t)regs);
            lapic_eoi();
            return new_rsp; 
        } else {
            for (int i = 0; i < 512; i++) {
                if ((uint64_t)handles[i].vector == vector && handles[i].intr != NULL) {
                    handles[i].intr(regs);
                    break;
                }
            }
            lapic_eoi();
        }
    }
    return 0;
}
