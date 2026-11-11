@ Idle-skip regression (docs/idle-skip.md), the listener: a CC2538 that
@ strobes ISRXON and polls FSMSTAT1.CCA, printing 'B' when it turns busy and
@ 'C' when it turns clear.  The other node (tx.s) sends a frame every few
@ milliseconds, so CCA follows the channel's energy.  Idle skip takes
@ FSMSTAT1 as a pure read; that holds because a window's start reaches the
@ node as an input and its end as an event (arm_elf_mote.c, air edges).
@ Build, in this directory (Arm GNU Toolchain 15.2.Rel1;
@ firmware/cc2538dk/PROVENANCE.md):
@   arm-none-eabi-as -mcpu=cortex-m3 -mthumb -o idle-cca-busy-rx.o rx.s
@   arm-none-eabi-ld -T link.ld -o ../../../firmware/cc2538dk/test-idle-cca-busy-rx.cc2538dk idle-cca-busy-rx.o
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
    ldr r5, =0x4008864C      @ FSMSTAT1
    ldr r6, =0x4000C000      @ UART0 DR
    ldr r0, =0x301
    str r0, [r6, #0x30]      @ UART0 CTL: UARTEN | TXE | RXE
    movs r0, #0xE3
    str r0, [r4]             @ ISRXON
    movs r7, #0              @ the CCA last seen: not clear
    movs r2, #1
poll:
    ldr r0, [r5]
    lsrs r0, r0, #4
    ands r0, r2              @ CCA (bit 4)
    cmp r0, r7
    beq poll
    mov r7, r0
    movs r0, #'B'
    cmp r7, #0
    beq print
    movs r0, #'C'
print:
    str r0, [r6]
    movs r0, #'\n'
    str r0, [r6]
    b poll
    .ltorg
    .section .cca, "a"
    .word 0xF0FFFFFF         @ bootloader backdoor disabled
    .word 0                  @ image valid
    .word 0x00200000         @ vector table
