.section .text
.align 16
.globl _start
.extern main
.weak virtua
.set virtua, main
.extern minos_current_user_abi
.extern __init_array_start
.extern __init_array_end
.extern __fini_array_start
.extern __fini_array_end
.extern sys_exit
_start:
    movq %r12, minos_current_user_abi(%rip)
    movq %rdi, %r14
    movq %rsi, %r15
    xorq %rbp, %rbp
    andq $-16, %rsp

    leaq __init_array_start(%rip), %rbx
    leaq __init_array_end(%rip), %r13
.Linit_array_loop:
    cmpq %r13, %rbx
    je .Linit_array_done
    call *(%rbx)
    addq $8, %rbx
    jmp .Linit_array_loop
.Linit_array_done:

    movq %r14, %rdi
    movq %r15, %rsi
    call virtua
    movl %eax, %r12d

    leaq __fini_array_start(%rip), %r13
    leaq __fini_array_end(%rip), %rbx
.Lfini_array_loop:
    cmpq %r13, %rbx
    je .Lfini_array_done
    subq $8, %rbx
    call *(%rbx)
    jmp .Lfini_array_loop
.Lfini_array_done:

    movl %r12d, %edi
    call sys_exit
.Lexit_spin:
    pause
    jmp .Lexit_spin
