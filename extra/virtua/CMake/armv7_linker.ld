OUTPUT_ARCH(arm)
ENTRY(_start)

SECTIONS
{
    . = 0;

    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
    }

    .ARM.exidx : {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    }

    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) .fini_array))
        PROVIDE_HIDDEN(__fini_array_end = .);
    }

    .data : {
        *(.data .data.*)
        *(.got .got.*)
        *(.got.plt)
    }

    .bss (NOLOAD) : {
        *(.bss .bss.*)
        *(COMMON)
    }

    . = ALIGN(64);
    PROVIDE(_end = .);
    PROVIDE(_heap_start = .);
    PROVIDE_HIDDEN(__llvm_libc_heap_limit = _end + 0x100000);
}
