ENTRY(reset)
SECTIONS {
  . = 0x0;
  .vectors : { KEEP(*(.vectors)) }
  .text : { *(.text*) }
}
