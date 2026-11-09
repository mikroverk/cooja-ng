@ Idle-skip regression: after a SysTick-ended poll loop, BLX to 0xFFFFFFFF (a
@ function pointer read from erased flash) puts PC at 0xFFFFFFFE.  A dry run
@ there wrapped pc + 2 to 0 in idle_jit_ok and read the host's flash array
@ 4 GiB out of range (SIGBUS with idle_skip on; found in review).
@ Build, in this directory (byte-identical with Arm GNU Toolchain 15.2.Rel1;
@ firmware/nrf52840-dk/PROVENANCE.md):
@   arm-none-eabi-as -mcpu=cortex-m4 -mthumb -o idle-blx-wild.o main.s
@   arm-none-eabi-ld -T ../idle-rtc-ovrflw/link.ld -o ../../../firmware/nrf52840-dk/test-idle-blx-wild.nrf52840-dk idle-blx-wild.o
    .syntax unified
    .cpu cortex-m4
    .thumb
    .section .vectors, "a"
    .word 0x20040000
    .word reset + 1
    .rept 13
    .word dflt + 1
    .endr
    .word systick_isr + 1
    .rept 48
    .word dflt + 1
    .endr
    .text
    .thumb_func
    .global reset
reset:
    b main
    .thumb_func
dflt:
    b dflt
    .thumb_func
main:
    ldr r5, =0x4000251C
    ldr r6, =0x20000100
    movs r0, #0
    str r0, [r6]
    movs r0, #'S'
    str r0, [r5]
    movs r0, #'\n'
    str r0, [r5]
    ldr r1, =0xE000E010
    ldr r0, =639999
    str r0, [r1, #4]
    movs r0, #0
    str r0, [r1, #8]
    movs r0, #7
    str r0, [r1]
    ldr r3, =0xFFFFFFFF
loop:
    ldr r0, [r6]
    cmp r0, #20
    blo loop
    .rept 900
    nop
    .endr
    blx r3
    b .
    .ltorg
    .thumb_func
systick_isr:
    ldr r1, =0x20000100
    ldr r0, [r1]
    adds r0, #1
    str r0, [r1]
    bx lr
    .ltorg
