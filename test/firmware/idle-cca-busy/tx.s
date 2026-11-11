@ Idle-skip regression (docs/idle-skip.md), the sender: a CC2538 that loads
@ a 20-byte frame, strobes ISTXON, prints 'T', waits a few milliseconds and
@ repeats, for rx.s to see CCA turn busy and clear again.
@ Build, in this directory (Arm GNU Toolchain 15.2.Rel1;
@ firmware/cc2538dk/PROVENANCE.md):
@   arm-none-eabi-as -mcpu=cortex-m3 -mthumb -o idle-cca-busy-tx.o tx.s
@   arm-none-eabi-ld -T link.ld -o ../../../firmware/cc2538dk/test-idle-cca-busy-tx.cc2538dk idle-cca-busy-tx.o
    .syntax unified
    .cpu cortex-m3
    .thumb
    .section .vectors, "a"
    .word 0x20004000
    .word reset + 1
    .rept 14
    .word dflt + 1
    .endr
    .text
    .thumb_func
dflt:
    b dflt
    .thumb_func
    .global reset
reset:
    ldr r4, =0x40088838      @ RFST
    ldr r5, =0x40088828      @ RFDATA
    ldr r6, =0x4000C000      @ UART0 DR
    ldr r0, =0x301
    str r0, [r6, #0x30]      @ UART0 CTL: UARTEN | TXE | RXE
    ldr r3, =9000            @ the first gap
again:
    movs r0, #0xEE
    str r0, [r4]             @ ISFLUSHTX
    movs r0, #22
    str r0, [r5]             @ length: 20 bytes + FCS
    movs r1, #20
fill:
    movs r0, #0x41
    str r0, [r5]
    subs r1, #1
    bne fill
    movs r0, #0xE9
    str r0, [r4]             @ ISTXON
    movs r0, #'T'
    str r0, [r6]
    movs r0, #'\n'
    str r0, [r6]
    mov r1, r3
delay:
    subs r1, #1
    bne delay
    ldr r0, =7000            @ gaps of 9000, 16000, 23000 ... loops, wrapping
    adds r3, r0
    ldr r0, =40000
    cmp r3, r0
    blo again
    ldr r3, =9000
    b again
    .ltorg
    .section .cca, "a"
    .word 0xF0FFFFFF         @ bootloader backdoor disabled
    .word 0                  @ image valid
    .word 0x00200000         @ vector table
