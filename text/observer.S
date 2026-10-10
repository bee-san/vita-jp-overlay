/* ARMv7 observer used for ARM and Thumb hooks. Save all integer/VFP registers,
 * original SP/LR, APSR including GE, and FPSCR. Tail-jump to taiHEN's next
 * hook/trampoline, preserving the original calling convention and stack.
 * Callback receives slot, r0..r12/SP/LR/continuation/APSR/FPSCR (18 words).
 * Instruction relocation remains taiHEN's responsibility. Do not hook in an
 * IT block or a site with branches into the overwritten instruction window. */
.syntax unified
.arch armv7-a
.fpu neon
.arm
.text
.align 2
.macro OBSERVER slot
.global vjo_observer_\slot
.type vjo_observer_\slot, %function
vjo_observer_\slot:
    sub sp, sp, #72
    stmia sp, {r0-r12}
    add r0, sp, #72
    str r0, [sp, #52]
    str lr, [sp, #56]
    mrs r0, APSR
    str r0, [sp, #64]
    vmrs r0, fpscr
    str r0, [sp, #68]
    vpush {d0-d15}
    vpush {d16-d31}
    mov r4, sp
    bic sp, sp, #7
    mov r0, #\slot
    add r1, r4, #256
    bl vjo_text_observe
    str r0, [r4, #316]
    mov sp, r4
    vpop {d16-d31}
    vpop {d0-d15}
    ldr r0, [sp, #68]
    vmsr fpscr, r0
    ldr r0, [sp, #64]
    msr APSR_nzcvqg, r0
    ldmia sp, {r0-r12}
    ldr lr, [sp, #56]
    add sp, sp, #60
    ldr pc, [sp], #12
.size vjo_observer_\slot, .-vjo_observer_\slot
.endm
OBSERVER 0
OBSERVER 1
OBSERVER 2
OBSERVER 3
OBSERVER 4
OBSERVER 5
OBSERVER 6
OBSERVER 7
.section .note.GNU-stack,"",%progbits
