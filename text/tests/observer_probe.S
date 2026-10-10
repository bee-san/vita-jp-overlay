/* Drive the shipping observer from ARM and Thumb call sites. The Thumb case
 * deliberately has a four-byte-aligned SP to exercise callback realignment. */
.syntax unified
.arch armv7-a
.fpu neon
.text
.macro PROBE name, thumb
.align 2
.if \thumb
.thumb
.thumb_func
.else
.arm
.endif
.global \name
.type \name, %function
\name:
    push {r4-r12,lr}
    vpush {d8-d15} /* preserve the probe caller's AAPCS callee-saved registers */
.if \thumb
    sub sp, sp, #4
.endif
    ldr r0, =vjo_probe_expected_sp
    mov r1, sp
    str r1, [r0]
    ldr r0, =vjo_probe_fp_initial
    vldmia r0!, {d0-d15}
    vldmia r0!, {d16-d31}
    movw r0, #0
    movt r0, #0x0200
    vmsr fpscr, r0
    movw r0, #0
    movt r0, #0x600F
    msr APSR_nzcvqg, r0
    movw r0, #1
    movw r1, #2
    movw r2, #3
    movw r3, #4
    movw r4, #5
    movw r5, #6
    movw r6, #7
    movw r7, #8
    movw r8, #9
    movw r9, #10
    movw r10, #11
    movw r11, #12
    movw r12, #13
.if \thumb
    blx vjo_observer_0
    add sp, sp, #4
.else
    bl vjo_observer_0
.endif
    vpop {d8-d15}
    pop {r4-r12,pc}
.size \name, .-\name
.ltorg
.endm
PROBE vjo_probe_arm, 0
PROBE vjo_probe_thumb, 1

.arm
.align 2
.global vjo_probe_continue_arm
.type vjo_probe_continue_arm, %function
vjo_probe_continue_arm:
    push {r0-r12,lr}
    mrs r2, APSR
    ldr r0, =vjo_probe_gp_captured
    mov r1, sp
    mov r3, #14
1:
    ldr r12, [r1], #4
    str r12, [r0], #4
    subs r3, r3, #1
    bne 1b
    ldr r0, =vjo_probe_flags_captured
    str r2, [r0]
    ldr r0, =vjo_probe_sp_captured
    add r1, sp, #56
    str r1, [r0]
    ldr r0, =vjo_probe_fp_captured
    vstmia r0!, {d0-d15}
    vstmia r0!, {d16-d31}
    vmrs r1, fpscr
    ldr r0, =vjo_probe_fpscr_captured
    str r1, [r0]
    pop {r0-r12,lr}
    bx lr
.size vjo_probe_continue_arm, .-vjo_probe_continue_arm
.ltorg

.thumb
.align 2
.thumb_func
.global vjo_probe_continue_thumb
.type vjo_probe_continue_thumb, %function
vjo_probe_continue_thumb:
    /* Interworking veneer without changing any GP register or flags. */
    ldr.w pc, [pc, #0]
    .word vjo_probe_continue_arm
.size vjo_probe_continue_thumb, .-vjo_probe_continue_thumb
.section .note.GNU-stack,"",%progbits
