ENTRY(kernel_entry)

SECTIONS
{
  /* Keep kernel image in low memory; BIOS jumps to ELF e_entry. */
  . = 0x00100000;
  __kernel_start = .;

  .text ALIGN(0x1000) : {
    __text_start = .;
    *(.text .text.*)
    __text_end = .;
  }

  .rodata ALIGN(0x1000) : {
    __rodata_start = .;
    *(.rodata .rodata.*)
    __rodata_end = .;
  }

  .data ALIGN(0x1000) : {
    __data_start = .;
    *(.data .data.*)
    __data_end = .;
  }

  /*
   * Keep the two firmware/ABI pages immediately below 3 MiB out of all
   * kernel sections.  The legacy syscall result mailbox lives at 0x2fe000
   * and BIOS republishes BootInfo at 0x2ff000 after loading the ELF.  Letting
   * an ordinary .bss allocation drift across either page makes the failure
   * depend on link order (and previously corrupted an MMU L2 table).
   *
   * BIOS deliberately tolerates BSS zeroing its temporary ELF buffer at
   * 0x300000 after the file-backed bytes have been copied.
   */
  ASSERT(__data_end <= 0x002FE000,
         "kernel file-backed sections overlap syscall ABI/BootInfo pages")
  . = MAX(., 0x00300000);

  .bss ALIGN(0x1000) : {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    __bss_end = .;
  }

  __kernel_end = .;
}
