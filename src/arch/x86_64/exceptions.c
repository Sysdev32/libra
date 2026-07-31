#include <stdint.h>
#include <drivers/fb.h>
#include <arch/x86_64/idt.h>
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
    return 0;
}
