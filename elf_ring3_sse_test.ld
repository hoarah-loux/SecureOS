/* Same two-PT_LOAD shape as elf_ring3_mmap_test.ld: read+exec for .text, a
 * separate writable one for .data/.bss. The SSE test needs both - .data holds
 * the seed patterns and the double-precision constants, .bss the spill area
 * the xmm registers are written back to. */
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
