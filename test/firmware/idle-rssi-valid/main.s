@ Idle-skip regression (docs/idle-skip.md): a CC2538 loop that strobes ISRXON
@ and busy-waits on RSSISTAT.RSSI_VALID, as Contiki-NG's cc2538-rf.c
@ channel_clear() does, prints 'V', strobes ISRFOFF, waits, and repeats.
@ Where RSSI_VALID rises a time after the receiver starts (the chip: 8 symbol
@ periods after calibration) with no event, a skip that took RSSISTAT as a
@ pure read ran the wait loop on past it.  RSSISTAT is refused.
@ Build, in this directory (Arm GNU Toolchain 15.2.Rel1;
@ firmware/cc2538dk/PROVENANCE.md):
@   arm-none-eabi-as -mcpu=cortex-m3 -mthumb -o idle-rssi-valid.o main.s
@   arm-none-eabi-ld -T link.ld -o ../../../firmware/cc2538dk/test-idle-rssi-valid.cc2538dk idle-rssi-valid.o
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
    ldr r5, =0x40088664      @ RSSISTAT
    ldr r6, =0x4000C000      @ UART0 DR
    ldr r0, =0x301
    str r0, [r6, #0x30]      @ UART0 CTL: UARTEN | TXE | RXE
again:
    movs r0, #0xE3
    str r0, [r4]             @ ISRXON
wait:
    ldr r0, [r5]
    lsls r0, r0, #31
    beq wait
    movs r0, #'V'
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
