/* Two PT_LOADs on purpose: a read+exec one for .text and a separate writable
 * one for .data/.bss, so the loader has to split a segment into a file-backed
 * region and an anonymous .bss region. The ALIGN(0x1000) between them keeps
 * each segment's file offset congruent to its virtual address modulo the page
 * size, which is what demand paging straight off the file requires. */
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
