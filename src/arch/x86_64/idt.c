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

#include "drivers/tty.h"
#include "drivers/usb/hid/keyboard.h"
#include "hals/ps2.h"
volatile uint64_t ticks = 0;
extern struct flanterm_context *ft_ctx;
extern uint64_t set_signal_handler(int sig, uint64_t handler);
extern int send_signal(int pid, int sig);
uint64_t admin_seed = 0xCAFEF00DD1CE1ULL; 
uint64_t user_seed = 0xCAFEF11DEADBEULL;
typedef void (*interrupt)(struct InterruptRegisters *regs);
uint64_t admin_key;
uint64_t user_key;
typedef struct {
    interrupt intr;
    int vector;
} handle;

// --- GLOBAL VARIABLES AND CONFIGURATION TRACKING ---
handle handles[512] = {};
extern void idt_load(struct IDTPtr *ptr);
extern void intr(void);
extern void *pmm_alloc_pages(int order);
extern uint64_t exception_vector_table[34]; // Expanded to hold all 34 elements (0-33)

static struct IDTEntry idt[256];
static struct IDTPtr   idt_ptr;

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
struct InterruptRegisters *current_intr;
static void handle_syscall(struct InterruptRegisters *regs) {
    arg *args = (arg*)regs->rbx;
    current_intr = regs;
    regs->rax = systable[regs->rax](args);
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
        keyboard_dispatch();
        lapic_eoi();
        return 0;
    }
    if (vector == 44) {
        mouse_dispatch();
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
void sleep_ms(uint64_t ms)
{
    uint64_t tickst = ms / 10;

    uint64_t start = ticks;

    while ((ticks - start) < tickst) {

        __asm__ volatile("hlt"); // low power wait until next interrupt
    }
}
