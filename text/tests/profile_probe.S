/* Synthetic hook sites with relocatable instructions, never game code. */
.syntax unified
.arch armv7-a
.text
.arm
.align 2
.global vjo_profile_arm
.type vjo_profile_arm, %function
vjo_profile_arm:
    nop
    nop
    mov r0, #17
    bx lr
.size vjo_profile_arm, .-vjo_profile_arm

.thumb
.align 2
.thumb_func
.global vjo_profile_thumb
.type vjo_profile_thumb, %function
vjo_profile_thumb:
    nop
    nop
    nop
    nop
    mov.w r0, #19
    bx lr
.size vjo_profile_thumb, .-vjo_profile_thumb

.arm
.align 2
.global vjo_profile_rejected
.type vjo_profile_rejected, %function
vjo_profile_rejected:
    nop
    nop
    mov r0, #23
    bx lr
.size vjo_profile_rejected, .-vjo_profile_rejected
.section .note.GNU-stack,"",%progbits
