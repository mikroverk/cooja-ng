ENTRY(reset)
SECTIONS {
  . = 0x00200000;
  .vectors : { KEEP(*(.vectors)) }
  .text : { *(.text*) }
  .cca 0x0027FFD4 : { KEEP(*(.cca)) }
}
