.section .text
.align 4
.globl _start
.extern main
.weak virtua
.set virtua, main
_start:
    .option push
    .option norelax
    la gp, __global_pointer$   # Set GP properly
    .option pop

    addi sp, sp, -32
    sd   ra, 24(sp)
    sd   s0, 16(sp)
    addi s0, sp, 32

    li   a0, 0
    li   a1, 0
    call virtua

    # main() returned -- hand the exit status to the kernel rather than
    # `ret`ing into whatever ra happens to be. The Minos U-mode context
    # switch zeroes ra before sret so ret'ing here used to cause an
    # instruction-access fault at PC=0, which looked like a kernel
    # panic. Use the Linux-compatible exit_group (93) and fall back to
    # legacy _exit (1); finally spin on ebreak so a mis-returned ecall
    # traps deterministically.
    mv   a0, a0                # main's return value = exit status
    li   a7, 93                # SYS_exit_group
    ecall
    li   a7, 1                 # SYS_exit
    ecall
1:  ebreak
    j    1b
