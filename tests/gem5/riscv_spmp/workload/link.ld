OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { ram (rwx) : ORIGIN = 0x80000000, LENGTH = 32M }
SECTIONS {
  .text.boot 0x80000000 : { *(.text.boot) } > ram
  .scode 0x80001000 : { __scode_start = .; *(.scode) } > ram
  .ucode 0x80002000 : { __ucode_start = .; *(.ucode) } > ram
  .spmp_data 0x80006000 : {
    __spmp_data_start = .;
    KEEP(*(.spmp_target_code))
    . = ALIGN(4);
    KEEP(*(.spmp_data.target))
    KEEP(*(.spmp_data.status))
  } > ram
  .spmp_split 0x8000603c : { KEEP(*(.spmp_split)) } > ram
  .spmp_locked_data 0x80008000 : {
    __spmp_locked_start = .; KEEP(*(.spmp_locked_data))
  } > ram
  .pagetable 0x80009000 : { KEEP(*(.pagetable)) } > ram
  .text 0x80010000 : {
    *(.text.mprobe) *(.text.trap) *(.text .text.*)
    *(.rodata .rodata.* .srodata .srodata.*)
  } > ram
  .data : { *(.data .data.* .sdata .sdata.*) } > ram
  .bss (NOLOAD) : {
    __bss_start = .; *(.bss .bss.* .sbss .sbss.* COMMON) __bss_end = .;
  } > ram
  .stack 0x81fff000 (NOLOAD) : { . += 0x1000; _stack_top = .; } > ram
  ASSERT(SIZEOF(.text.boot) <= 0x1000, "boot overlaps S probes")
  ASSERT(SIZEOF(.scode) <= 0x1000, "S probes exceed page")
  ASSERT(SIZEOF(.ucode) <= 0x1000, "U probes exceed page")
  ASSERT(SIZEOF(.spmp_data) <= 0x3c, "target exceeds PMA test page")
  ASSERT(SIZEOF(.pagetable) <= 0x1000, "root page table exceeds page")
  ASSERT(__bss_end <= 0x81fff000, "workload overlaps stack")
  /DISCARD/ : { *(.comment) *(.eh_frame*) *(.riscv.attributes) }
}
