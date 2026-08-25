/* Same two-PT_LOAD shape as elf_ring3_mmap_test.ld: read+exec for .text, a
 * separate writable one for .data/.bss. The SSE test needs both - .data holds
 * the seed patterns and the double-precision constants, .bss the spill area
 * the xmm registers are written back to. */
/* Without this, ld picks the entry point by falling back to the start of
 * .text, which is only ever _start by accident of source order - and stops
 * being _start the moment a helper function is defined above it. A binary
 * entered at the wrong symbol runs with a zeroed register file and dies
 * somewhere that looks nothing like the cause; elf_ring3_time_test.S was
 * entered at its serial-print helper and faulted on the `ret` of a call that
 * had never happened. */
ENTRY(_start)

PHDRS
{
  headers PT_PHDR PHDRS ;
  text PT_LOAD FILEHDR PHDRS FLAGS(5) ;   /* R E */
  data PT_LOAD FLAGS(6) ;                 /* RW  */
}

SECTIONS
{
  . = 0x400000 + SIZEOF_HEADERS;
  .text : { *(.text) } :text

  . = ALIGN(0x1000);
  .data : { *(.data) } :data
  .bss  : { *(.bss) *(COMMON) } :data

  /DISCARD/ : { *(*) }
}
