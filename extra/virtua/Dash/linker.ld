/*
 * RISC-V Linker Script for Relocatable/PIE Modules
 */

OUTPUT_ARCH( "riscv" )
ENTRY(_start)

/* Force the linker to keep this symbol in the symbol table */
EXTERN(__global_pointer$)

/* Stack size is only for linker checking */
PROVIDE( _stack_size = 16K );

MEMORY
{
  /* Attributes (wxa!ri) are accepted by GNU ld but not by lld; omit
   * them — they are advisory only and do not affect placement. */
  ram : ORIGIN = 0x00000000, LENGTH = 4M
}

PHDRS
{
  text_ro PT_LOAD FLAGS(0x5); /* R+X */
  data_rw PT_LOAD FLAGS(0x6); /* R+W */
  bss_rw  PT_LOAD FLAGS(0x6); /* R+W */
}

SECTIONS
{
  /* --- Code & Read-Only Data --- */
  .text :
  {
    PROVIDE( _text = . );
    KEEP( *(.text.start) )
    *(.text .text.*)
    *(.gnu.linkonce.t.*)
    . = ALIGN(8);
    PROVIDE( _etext = . );
  } >ram :text_ro

  .rodata :
  {
    PROVIDE( _rodata = . );
    *(.rdata)
    *(.rodata .rodata.*)
    *(.gnu.linkonce.r.*)
    . = ALIGN(8);
    PROVIDE( _erodata = . );
  } >ram :text_ro

  /* --- Initialized Data --- */
  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >ram :data_rw

  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) .fini_array))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >ram :data_rw

  . = ALIGN(4096);
  
  .data :
  {
    PROVIDE( _data = . );
    
    /* * Global Pointer (gp) optimization.
     * We place this at the start of data.
     * REMOVED 'PROVIDE' to force the definition.
     */
    . = ALIGN(8);
    __global_pointer$ = . + 0x800;

    *(.data .data.*)
    *(.sdata .sdata.*)
    *(.got)
    *(.got.plt)

    . = ALIGN(8);
    PROVIDE( _edata = . );
  } >ram :data_rw

  /* --- Uninitialized Data (BSS) --- */
  .bss :
  {
    PROVIDE( _bss = . );
    *(.sbss .sbss.*)
    *(.gnu.linkonce.sb.*)
    *(.scommon)
    *(.bss .bss.*)
    *(.gnu.linkonce.b.*)
    *(COMMON)
    . = ALIGN(8);
    PROVIDE( _ebss = . );
  } >ram :bss_rw

  /* --- Heap Markers --- */
  . = ALIGN(64);
  PROVIDE( _end = . );
  PROVIDE( _heap_start = . );
  /* llvm-libc's freelist heap expects linker-style byte symbols.  Point its
   * heap limit one process-heap window past _end so the runtime can use the
   * zeroed memory the loader reserves after the image. */
  PROVIDE_HIDDEN( __llvm_libc_heap_limit = _end + 0x100000 );
}
