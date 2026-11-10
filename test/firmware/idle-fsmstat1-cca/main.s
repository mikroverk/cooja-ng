@ Idle-skip regression (docs/idle-skip.md): a CC2538 loop that strobes ISRXON
@ and busy-waits on FSMSTAT1.CCA, as a CSMA driver checking the channel does
@ before it transmits, prints 'C', strobes ISRFOFF, waits, and repeats.
@ CCA reads clear once RSSI is valid, 8 symbol periods after calibration.
@ FSMSTAT1 is a pure read for idle skip (cc2538_idle_io), which holds only
@ because that rise is an event (rssi_event); without it a skip ran the wait
@ loop on past the rise.
@ Build, in this directory (Arm GNU Toolchain 15.2.Rel1;
@ firmware/cc2538dk/PROVENANCE.md):
@   arm-none-eabi-as -mcpu=cortex-m3 -mthumb -o idle-fsmstat1-cca.o main.s
@   arm-none-eabi-ld -T link.ld -o ../../../firmware/cc2538dk/test-idle-fsmstat1-cca.cc2538dk idle-fsmstat1-cca.o
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
again:
    movs r0, #0xE3
    str r0, [r4]             @ ISRXON
wait:
    ldr r0, [r5]
    lsls r0, r0, #27         @ CCA (bit 4) into N
    bpl wait
    movs r0, #'C'
    str r0, [r6]
    movs r0, #'\n'
    str r0, [r6]
    movs r0, #0xEF
    str r0, [r4]             @ ISRFOFF
    ldr r1, =20000
delay:
    subs r1, #1
    bne delay
    b again
    .ltorg
    .section .cca, "a"
    .word 0xF0FFFFFF         @ bootloader backdoor disabled
    .word 0                  @ image valid
    .word 0x00200000         @ vector table
