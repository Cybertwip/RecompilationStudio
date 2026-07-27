.section .text
.syntax unified
.arm
.align 4
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
    ldr r2, =minos_current_user_abi
    str r12, [r2]
    mov r4, r0
    mov r5, r1
    mov r11, #0
    bic sp, sp, #7

    ldr r6, =__init_array_start
    ldr r7, =__init_array_end
.Linit_array_loop:
    cmp r6, r7
    beq .Linit_array_done
    ldr r3, [r6], #4
    blx r3
    b .Linit_array_loop
.Linit_array_done:

    mov r0, r4
    mov r1, r5
    bl virtua
    mov r4, r0

    ldr r6, =__fini_array_end
    ldr r7, =__fini_array_start
.Lfini_array_loop:
    cmp r6, r7
    beq .Lfini_array_done
    ldr r3, [r6, #-4]!
    blx r3
    b .Lfini_array_loop
.Lfini_array_done:

    mov r0, r4
    bl sys_exit
.Lexit_spin:
    yield
    b .Lexit_spin
