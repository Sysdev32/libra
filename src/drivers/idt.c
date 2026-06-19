// SPDX-License-Identifier: GPL-3.0-only
#include <drivers/idt.h>
#include <string.h>
#include <drivers/fb.h>
#include <limine.h>
#include <drivers/schedule.h>
#include <drivers/vfs.h>
#include <uacpi/uacpi.h>
#include <uacpi/sleep.h>
#include <stdint.h>
uint64_t admin_seed = 0xCAFEF00DD1CE1ULL; 
uint64_t user_seed = 0xCAFEF11DEADBEULL; 
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
    
    printk(LOG_TRACE, "IOAPIC mapped at virtual address: %p\n", (void*)ioapic_virtual_base);

    // Read the version register to verify communication
    uint32_t version_reg = ioapic_read(ioapic_virtual_base, 0x01);
    int max_entries = ((version_reg >> 16) & 0xFF) + 1;

    printk(LOG_TRACE, "IOAPIC verified. Max Redirection Entries: %d\n", max_entries);

    // Mask (disable) all redirection entries by default for safety
    for (int i = 0; i < max_entries; i++) {
        uint8_t reg_low = 0x10 + (i * 2);
        uint8_t reg_high = reg_low + 1;

        // Bit 16 = 1 masks the interrupt line
        ioapic_write(ioapic_virtual_base, reg_low, 0x00010000);
        ioapic_write(ioapic_virtual_base, reg_high, 0x00000000);
    }
    
    printk(LOG_TRACE, "All IOAPIC pins masked and ready for manual routing.\n");
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
                    printk(LOG_TRACE, "Found CPU Core - Processor ID: %d, APIC ID: %d\n", 
                           lapic->acpi_processor_id, lapic->apic_id);
                    apic[lapicint++] = *lapic;
                }   
                break;
            }
            case 1: {
                struct madt_io_apic *ioapic = (struct madt_io_apic *)record;
                printk(LOG_TRACE, "Found I/O APIC - ID: %d, Address: 0x%x, GSI Base: %d\n",
                       ioapic->io_apic_id, ioapic->io_apic_address, ioapic->gsi_base);
                ioapic_init(ioapic->io_apic_address);
                break;
            }
            case 2: {
                struct madt_interrupt_override *override = (struct madt_interrupt_override *)record;
                printk(LOG_TRACE, "Interrupt Override - IRQ Source %d -> GSI %d\n",
                       override->irq_source, override->gsi);
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

void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

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
    idt_set_descriptor(0x21, (void*)exception_vector_table[33], 0x8E);
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

    // 5. Initialize the scheduler structures (Sets up Task 0 as RUNNING)
    init_scheduler();
}
typedef struct {
    uint64_t key;
    int claimedlevel;
} permission;
// --- VIRTUAL FILE SYSTEM SYSTEM-CALL ISOLATED ROUTER ---
#include <stdint.h>
#include <string.h>

// --- Minimal Freestanding SHA-256 Implementation ---
#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define Ch(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define Maj(x, y, z) (((x) & (y)) ^ (((x) & (z)) ^ ((y) & (z))))
#define Sigma0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define Sigma1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define sigma0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define sigma1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0xbefa4fa4, 0x0654be30, 0x14abbc42, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb
};

void sha256_transform(uint32_t state[8], const uint8_t data[64]) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, W[64];
    int i;

    for (i = 0; i < 16; i++) {
        W[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) | ((uint32_t)data[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        W[i] = sigma1(W[i - 2]) + W[i - 7] + sigma0(W[i - 15]) + W[i - 16];
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + Sigma1(e) + Ch(e, f, g) + K[i] + W[i];
        t2 = Sigma0(a) + Maj(a, b, c);
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_hash(const uint8_t* data, uint32_t length, uint8_t output[32]) {
    uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buffer[128];
    uint32_t bits = length * 8;
    
    // Copy data into buffer and apply standard padding
    for (uint32_t i = 0; i < length; i++) buffer[i] = data[i];
    buffer[length] = 0x80;
    
    uint32_t pad_len = length + 1;
    while ((pad_len % 64) != 56) {
        buffer[pad_len++] = 0x00;
    }
    
    // Append bit length at the end (Big Endian)
    buffer[pad_len++] = (bits >> 24) & 0xFF;
    buffer[pad_len++] = (bits >> 16) & 0xFF;
    buffer[pad_len++] = (bits >> 8) & 0xFF;
    buffer[pad_len++] = bits & 0xFF;

    for (uint32_t i = 0; i < pad_len; i += 64) {
        sha256_transform(state, &buffer[i]);
    }

    for (int i = 0; i < 8; i++) {
        output[i * 4]     = (state[i] >> 24) & 0xFF;
        output[i * 4 + 1] = (state[i] >> 16) & 0xFF;
        output[i * 4 + 2] = (state[i] >> 8) & 0xFF;
        output[i * 4 + 3] = state[i] & 0xFF;
    }
}

// --- Freestanding HMAC-SHA256 Function ---
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
uint64_t signature_to_uint64_direct(const uint8_t* signature) {
    uint64_t result;
    // Copies the first 8 bytes of the signature directly into the uint64_t
    // Works safely across alignment boundaries
    for (int i = 0; i < 8; i++) {
        ((uint8_t*)&result)[i] = signature[i];
    }
    return result;
}

static void handle_syscall(struct InterruptRegisters *regs) {
    arg *args = (arg*)regs->rbx;

    switch (regs->rax) {
        case 0: // vfs_read
            if (args->arg <= 2) {
                regs->rax = 0; 
            } else {
                regs->rax = vfs_read(args->arg[0] - 2, (void*)args->arg[1], args->arg[2], args->arg[3]);
            }
            break;

        case 1: // vfs_write_file
            if (args->arg > 2) {
                regs->rax = vfs_write_file(args->arg[0] - 2, (void*)args->arg[1], args->arg[2]);
            } 
            else if (args->arg == 1 || args->arg == 2) {
                char *user_str = (char*)args->arg[0];
                unsigned long length = args->arg[1];

                for (unsigned long i = 0; i < length; i++) {
                    printk(LOG_NONE, "%c", user_str[i]);
                }
                regs->rax = length; 
            }
            break;

        case 2: { // vfs_open
            long fd = vfs_open((const char*)args->arg[0]);
            if (fd < 0) {
                regs->rax = fd; 
            } else {
                regs->rax = fd + 2; 
            }
            break;
        }

        case 3: // vfs_mkdir
            regs->rax = vfs_mkdir((const char*)args->arg[0], args->arg[1]);
            break;

        case 4: // vfs_rmdir
            regs->rax = vfs_rmdir((const char*)args->arg[0]);
            break;

        case 5: // vfs_free_fd (_close)
            if (args->arg <= 2) {
                regs->rax = 0; 
            } else {
                regs->rax = vfs_free_fd(args->arg[0] - 2);
            }
            break;

        case 6: // vfs_move_file
            if (args->arg <= 2) {
                regs->rax = -1; 
            } else {
                regs->rax = vfs_move_file(args->arg[0] - 2, (const char*)args->arg[1]);
            }
            break;

        case 7: { // vfs_create_file
            long fd = vfs_create_file((void*)args->arg[0], (const char*)args->arg[1], args->arg[2]);
            if (fd < 0) {
                regs->rax = fd; 
            } else {
                regs->rax = fd + 2; 
            }
            break;
        }

        case 8: // vfs_delete_file
            regs->rax = vfs_delete_file((const char*)args->arg[0]);
            break;
        case 9: {
            // TODO: Contact launchd executable (level -1) and get permission for the process to gain administrator level
            // TODO: Fix unsafe buffer copy
            if (args->arg[0] == 0) {
                uint8_t signature[32];
                sign_key_with_pid((uint8_t*)admin_key, sizeof(admin_key), getpid(), signature); 
                permission perm = { .claimedlevel = 0, .key = signature_to_uint64_direct(signature)};
                memcpy((void*)args->arg[1], &perm, sizeof(permission));
            } else if (args->arg[0] == 1) {
                uint8_t signature[32];
                sign_key_with_pid((uint8_t*)user_key, sizeof(user_key), getpid(), signature); 
                permission perm = { .claimedlevel = 0, .key = signature_to_uint64_direct(signature)};
                memcpy((void*)args->arg[1], &perm, sizeof(permission));
            }
            break;
        }
        default: 
            regs->rax = -1; // Unknown syscall ID
            break;
    }
}

// --- COMMON HARDWARE AND LEGACY INTERRUPT DISPATCH ROUTER ---
volatile uint64_t ticks = 0;
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
        uint64_t new_rsp = schedule_preemptive((uint64_t)regs);
        lapic_eoi();
        return new_rsp;
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
char *strcpy(char *dest, const char *src)
{
    char *orig = dest;

    while (*src) {
        *dest++ = *src++;
    }

    *dest = '\0';

    return orig;
}
uint64_t exception_handler_c(struct InterruptRegisters *regs) {
    asm volatile ("sti");
    uint64_t vector = regs->int_no; 

    // --- CASE A: CRITICAL ARCHITECTURAL CPU EXCEPTIONS (0-31) ---
    if (vector < 32) {
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
        
        uacpi_reboot();
        // Force definitive crash freeze so hardware doesn't pass broken state steps downward
        for (;;) {
            asm volatile("hlt");
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
    printk(LOG_TRACE, "ey");
    return 0;
}