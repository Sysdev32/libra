bits 64
section .text

global _start
extern main
extern _exit

_start:
    ; 1. Establish the clean stack base frame for the ABI
    xor rbp, rbp        ; Zero the frame pointer to terminate backtraces
    
    ; 2. Fetch arguments from stack (placed by your ELF loader)
    ; Standard x86_64 SysV ABI entry layout:
    ; [rsp]     = argc
    ; [rsp + 8] = argv pointer array
    mov rdi, [rsp]      ; Argument 1 (rdi): argc
    lea rsi, [rsp + 8]  ; Argument 2 (rsi): argv

    ; 3. Invoke user code application entry point
    call main

    ; 4. Gracefully exit task if main finishes execution
    mov rdi, rax        ; Pass the integer return value of main to _exit
    call _exit

    ; Safety fallback loop
.hang:
    pause
    jmp .hang