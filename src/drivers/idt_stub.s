bits 64

global idt_load
global exception_vector_table
global intr

extern exception_handler_c
extern intrhandler
extern fpu_context_save
extern fpu_context_restore

section .text

; --- IDT REGISTER LOADER ---
; Loads the Interrupt Descriptor Table pointer into the CPU
idt_load:
    lidt [rdi]
    ret

; --- STUB GENERATION MACROS ---

; 1. For interrupts/exceptions that DO NOT push an error code automatically
%macro no_error_stub 1
exception_stub_%1:
    push qword 0         ; Push a dummy error code to keep the stack uniform
    push qword %1        ; Push the interrupt vector index
    jmp exception_common_stub
%endmacro

; 2. For exceptions that DO push an error code automatically (like #GP or #PF)
%macro error_stub 1
exception_stub_%1:
    push qword %1        ; Push the interrupt vector index
    jmp exception_common_stub
%endmacro

; --- GENERATE ALL 32 SYSTEM EXCEPTION LANDING PADS ---
no_error_stub 0   ; #DE (Divide-by-Zero)
no_error_stub 1   ; #DB (Debug)
no_error_stub 2   ; NMI (Non-Maskable Interrupt)
no_error_stub 3   ; #BP (Breakpoint)
no_error_stub 4   ; #OF (Overflow)
no_error_stub 5   ; #BR (Bound Range Exceeded)
no_error_stub 6   ; #UD (Invalid Opcode)
no_error_stub 7   ; #NM (Device Not Available)
error_stub    8   ; #DF (Double Fault)
no_error_stub 9   ; Coprocessor Segment Overrun
error_stub    10  ; #TS (Invalid TSS)
error_stub    11  ; #NP (Segment Not Present)
error_stub    12  ; #SS (Stack-Segment Fault)
error_stub    13  ; #GP (General Protection Fault)
error_stub    14  ; #PF (Page Fault)
no_error_stub 15  ; Reserved
no_error_stub 16  ; #MF (x87 Floating-Point)
error_stub    17  ; #AC (Alignment Check)
no_error_stub 18  ; #MC (Machine Check)
no_error_stub 19  ; #XM (SIMD Floating-Point)
no_error_stub 20  ; #VE (Virtualization)
error_stub    21  ; #CP (Control Protection)
no_error_stub 22  ; Reserved
no_error_stub 23  ; Reserved
no_error_stub 24  ; Reserved
no_error_stub 25  ; Reserved
no_error_stub 26  ; Reserved
no_error_stub 27  ; Reserved
no_error_stub 28  ; Hypervisor Injection
error_stub    29  ; #VC (VMM Communication)
error_stub    30  ; #SX (Security Exception)
no_error_stub 31  ; Reserved

; --- HARDWARE IRQ LANDING PADS (VECTORS 32+) ---

; Vector 32 (0x20) - Hardware Timer / PIT
align 16
exception_stub_32:
    push qword 0         ; Dummy error code
    push qword 32        ; Vector index
    jmp interrupt_common_stub

; Vector 33 (0x21) - PS/2 Keyboard
align 16
exception_stub_33:
    push qword 0         ; Dummy error code
    push qword 33        ; Vector index
    jmp interrupt_common_stub

; Legacy fallback hook / System Call hook
align 16
intr:
    push qword 0         ; Dummy error code
    push qword 128       ; Vector index (0x80)
    jmp interrupt_common_stub


; --- HARDWARE INTERRUPT ROUTER (Preemptive Scheduling Path) ---
align 16
interrupt_common_stub:
    ; Save all general-purpose registers to form a clean InterruptRegisters frame
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax

    call fpu_context_save
    mov rdi, rsp         ; Pass the pointer to this saved state as the 1st C argument
    call intrhandler     ; Run our timer, hardware routing, or system calls

    mov rbx, rax         ; Preserve the handler return value across the FPU restore
    call fpu_context_restore
    mov rax, rbx

    ; Context Switch Check: If the C handler returns 0, keep the current task
    cmp rax, 0
    je .no_irq_switch

    ; Apply the new stack pointer returned by the scheduler
    mov rsp, rax

.no_irq_switch:
    ; Restore all general-purpose registers from the target task's stack frame
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    add rsp, 16          ; Pop the vector number and dummy error code off the stack
    iretq                ; Return to the target task execution path


; --- CORE CPU EXCEPTION ROUTER ---
align 16
exception_common_stub:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax

    call fpu_context_save
    mov rdi, rsp
    call exception_handler_c

    mov rbx, rax
    call fpu_context_restore
    mov rax, rbx

    ; Exceptions shouldn't shift scheduling blocks under regular conditions,
    ; but if a fatal recovery frame returns a new address, switch stacks here.
    cmp rax, 0
    je .no_exc_switch
    mov rsp, rax

.no_exc_switch:
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    add rsp, 16
    iretq


; --- THE EXPORTED IDT POINTER VECTOR TABLE ---
section .data
align 8
exception_vector_table:
%assign i 0
%rep 34                   ; Generates entries 0 through 33 sequentially
    dq exception_stub_%[i]
%assign i i+1
%endrep

; Keep the stack configuration modern and standard
section .note.GNU-stack noalloc noexec nowrite progbits
