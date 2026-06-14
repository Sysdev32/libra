bits 64
global idt_load
global exception_vector_table
global intr
extern exception_handler_c
extern intrhandler
extern printk

section .text

; --- IDT REGISTER LOADER ---
idt_load:
    lidt [rdi]
    ret

; --- STUB GENERATION MACROS ---

; 1. For interrupts/exceptions that DO NOT push an error code
%macro no_error_stub 1
exception_stub_%1:
    push qword 0    ; Dummy error code
    push qword %1    ; Vector index
    jmp exception_common_stub
%endmacro

; 2. For exceptions that DO push an error code
%macro error_stub 1
exception_stub_%1:
    push qword %1    ; Vector index
    jmp exception_common_stub
%endmacro

; --- GENERATE ALL 32 EXCEPTION LANDING PADS ---
no_error_stub 0   ; #DE
no_error_stub 1
no_error_stub 2
no_error_stub 3
no_error_stub 4
no_error_stub 5
no_error_stub 6
no_error_stub 7
error_stub    8   ; #DF
no_error_stub 9
error_stub    10  ; #TS
error_stub    11  ; #NP
error_stub    12  ; #SS
error_stub    13  ; #GP
error_stub    14  ; #PF
no_error_stub 15
no_error_stub 16
error_stub    17  ; #AC
no_error_stub 18
no_error_stub 19
no_error_stub 20
error_stub    21  ; #CP
no_error_stub 22
no_error_stub 23
no_error_stub 24
no_error_stub 25
no_error_stub 26
no_error_stub 27
no_error_stub 28
error_stub    29  ; #VC
error_stub    30  ; #SX
no_error_stub 31

; --- IRQ LANDING PADS (VECTORS 32+) ---
; Vector 32 (0x20) - Hardware Timer
exception_stub_32:
    push qword 0    ; Dummy error code
    push qword 32   ; Vector index
    jmp interrupt_common_stub

; Legacy fallback hook
intr:
    push qword 0    ; Dummy error code
    push qword 32   ; Vector index
    jmp interrupt_common_stub


; --- HARDWARE INTERRUPT ROUTER (PREEMPTIVE Ticks) ---
interrupt_common_stub:
    ; 1. Save standard integer registers
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

    mov rdi, rsp        ; Pass context frame pointer to C
    call intrhandler    ; Run your timer/scheduler code

    ; Debug: print candidate stack pointer returned in RAX and current RSP
    lea rdi, [rel dbg_intr_fmt]
    mov rsi, rax
    mov rdx, rsp
    call printk

    ; Context Switch Check: If RAX == 0, skip stack modification
    cmp rax, 0
    je .no_irq_switch
    ; Validate returned stack pointer before switching:
    ; - Must be canonical higher-half kernel pointer (0xffff8000...)
    ; - Must be 16-byte aligned
    mov rcx, rax
    cmp rax, 0xffff800000000000
    jb .no_irq_switch    ; If below canonical kernel base, ignore
    test rax, 0xF
    jnz .no_irq_switch    ; If not 16-byte aligned, ignore
    mov rsp, rax         ; Load the new task's stack pointer safely
.no_irq_switch:

    ; 4. Restore integer registers
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

    add rsp, 16         ; Clean up dummy error code and vector index
    iretq


; --- CORE CPU EXCEPTION ROUTER ---
exception_common_stub:
    ; 1. Save standard integer registers
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

    mov rdi, rsp
    call exception_handler_c

    ; Debug: print candidate stack pointer returned in RAX and current RSP
    lea rdi, [rel dbg_exc_fmt]
    mov rsi, rax
    mov rdx, rsp
    call printk

    ; Exceptions should not implicitly alter scheduling unless requested
    cmp rax, 0
    je .no_exc_switch
    ; Validate returned stack pointer before switching for exceptions as well
    mov rcx, rax
    cmp rax, 0xffff800000000000
    jb .no_exc_switch
    test rax, 0xF
    jnz .no_exc_switch
    mov rsp, rax
.no_exc_switch:

    ; 4. Restore integer registers
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

    add rsp, 16         ; Clean up vector tracking elements
    iretq


; --- THE EXPORTED VECTOR TABLE ---
section .data
align 8
exception_vector_table:
%assign i 0
%rep 33
    dq exception_stub_%[i]
%assign i i+1
%endrep

; Debug format strings for interrupt/exception instrumentation
dbg_intr_fmt: db "[DBG] intr candidate rax=%p rsp=%p\n", 0
dbg_exc_fmt:  db "[DBG] exc  candidate rax=%p rsp=%p\n", 0

section .note.GNU-stack noalloc noexec nowrite progbits