OUTPUT_FORMAT(elf64-x86-64)
ENTRY(start)

PHDRS
{
  text PT_LOAD FLAGS(5); /* PF_R | PF_X */
  data PT_LOAD FLAGS(6); /* PF_R | PF_W */
}

SECTIONS {
  . = 0x00100000;

  _kernel_start = .;

  .multiboot ALIGN(8) : { KEEP(*(.multiboot)) } :text

  .text : {
    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
  } :text

  .data : {
    *(.data)
    *(.data*)
  } :data

  .bss : {
    *(.bss)
    *(.bss*)
    *(COMMON)
    
    . = ALIGN(16);
    _stack_start = .;
    . += 65536;        /* 64 KB stack */
    _stack_end = .;
    
    . = ALIGN(16);
    _tss_buffer = .;
    . += 104;          /* TSS */
  } :data

  .heap (NOLOAD) : {
    . = ALIGN(8);
    _heap_start = .;
    . = . + 512 * 1024 * 1024;  /* 512 MB кучи */
    _heap_end = .;
  } :data

  _kernel_end = .;
}
