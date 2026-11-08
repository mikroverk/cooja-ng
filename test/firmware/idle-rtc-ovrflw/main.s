@ Idle-skip regression (docs/idle-skip.md, arm_idle.h STORE contract): an nRF52840
@ RTC0 overflow interrupt that a loop unmasks for part of each pass.
@ PRESCALER 4095, TRIGOVRFLW (counter 0xFFFFF0: the wrap ~2 s in), then a loop of
@ INTENSET=OVRFLW; 2 nop; INTENCLR=OVRFLW; 40 nop; poll a RAM flag the ISR sets.
@ The ISR fires while the loop has the interrupt unmasked; the loop prints 'O'.
@ With 42 NOPs before the loop, a skip that took the next event at a masked
@ point ran past the wrap and lost the interrupt (fixed: INTEN stores refused).
@ Build: arm-none-eabi-as -mcpu=cortex-m4 -mthumb -o m.o main.s
@        arm-none-eabi-ld -T link.ld -o idle-rtc-ovrflw.elf m.o
    .syntax unified
    .cpu cortex-m4
    .thumb
    .section .vectors, "a"
    .word 0x20040000
    .word reset + 1
    .rept 25
    .word dflt + 1
    .endr
    .word rtc0_isr + 1
    .rept 36
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
    .global main
main:
    ldr r4, =0x4000B000
    ldr r5, =0x4000251C
    ldr r6, =0x20000100
    movs r0, #0
    str r0, [r6]
    movs r0, #'S'
    str r0, [r5]
    movs r0, #'\n'
    str r0, [r5]
    ldr r0, =4095
    str.w r0, [r4, #0x508]
    movs r0, #1
    str.w r0, [r4, #0x00C]
    ldr r1, =0xE000E100
    mov.w r0, #(1<<11)
    str r0, [r1]
    movs r0, #1
    str r0, [r4, #0]
    movs r7, #2
    add.w r8, r4, #0x304
    add.w r9, r4, #0x308
    .rept 42
    nop
    .endr
loop:
    str r7, [r8]
    .rept 2
    nop
    .endr
    str r7, [r9]
    .rept 40
    nop
    .endr
    ldr r0, [r6]
    cmp r0, #0
    beq loop
    movs r0, #0
    str r0, [r6]
    movs r0, #'O'
    str r0, [r5]
    movs r0, #'\n'
    str r0, [r5]
    b loop
    .ltorg
    .thumb_func
rtc0_isr:
    ldr r1, =0x4000B104
    movs r0, #0
    str r0, [r1]
    ldr r1, =0x20000100
    movs r0, #1
    str r0, [r1]
    bx lr
    .ltorg
