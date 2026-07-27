.syntax unified
.arm
.fpu vfpv3
.text
.align 4
.global psx_virtua_fiber_swap
.type psx_virtua_fiber_swap,%function
psx_virtua_fiber_swap:
    stmia r0!, {r4-r11}
    str sp, [r0], #4
    str lr, [r0], #4
    vstmia r0!, {d8-d15}

    ldmia r1!, {r4-r11}
    ldr sp, [r1], #4
    ldr lr, [r1], #4
    vldmia r1!, {d8-d15}
    bx lr
.size psx_virtua_fiber_swap, .-psx_virtua_fiber_swap
