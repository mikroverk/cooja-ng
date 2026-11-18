/* csim TI SimpleLink stand-in ROM (CC13x2/CC26x2 and CC26x0): 0x10000000,
 * the chip's ROM window.  The two fixed addresses are the ones
 * driverlib/rom.h calls through. */
MEMORY
{
    ROM (rx) : ORIGIN = 0x10000000, LENGTH = 256K
}

SECTIONS
{
    .rom_head 0x10000000 : { LONG(0) LONG(0) } > ROM   /* no boot code */
    .rom_hapi 0x10000048 : { KEEP(*(.rom_hapi)) } > ROM
    ASSERT(. <= 0x10000180, "HAPI table overlaps ROM_API_TABLE")
    .rom_api_table 0x10000180 : { KEEP(*(.rom_api_table)) } > ROM
    .text : ALIGN(4)
    {
        KEEP(*(.rom_tables))
        *(.text .text.*)
        *(.rodata .rodata.*)
    } > ROM
    /* ROM has no writable data: anything reachable that needs some is an
     * error, not something to drop. */
    .ram (NOLOAD) : { *(.data .data.* .bss .bss.* COMMON) }
    ASSERT(SIZEOF(.ram) == 0, "ROM code needs .data/.bss")
    /DISCARD/ : { *(.ARM.exidx* .comment) }
    ASSERT(SIZEOF(.text) > 0, "empty ROM")
}
