/* One read+exec PT_LOAD holding both .text and the 20 KiB .pages payload.
 * The ALIGN(0x1000) is what gives .pages a page-aligned base, which the .org
 * offsets in elf_ring3_big_test.S rely on to place markers at an exact 4096
 * stride and to straddle a page boundary on purpose. Doing that alignment with
 * .balign inside .text instead would raise the output section's own alignment
 * and push it off the file-offset congruence that FILEHDR PHDRS in this same
 * segment needs. */
PHDRS
{
  headers PT_PHDR PHDRS ;
  text PT_LOAD FILEHDR PHDRS FLAGS(5) ;   /* R E */
}

SECTIONS
{
  . = 0x400000 + SIZEOF_HEADERS;
  .text : { *(.text) } :text

  . = ALIGN(0x1000);
  .pages : { *(.pages) } :text

  /DISCARD/ : { *(*) }
}
