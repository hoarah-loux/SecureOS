/* Linker script for SecureOS userland C binaries.
 *
 * Two PT_LOADs: a read+exec one for .text/.rodata and a writable one for
 * .data/.bss. The loader maps segments straight off the file with no
 * relocation step (elf_map_segments in src/kernel/proc/loader/loader.c), so
 * three things here are load-bearing rather than stylistic:
 *
 *   - Each segment's file offset must be congruent to its virtual address
 *     modulo 4096. loader.c rejects a phdr where that does not hold, because
 *     demand paging computes the file offset of a page from its address. The
 *     ALIGN(0x1000) before .data plus -z max-page-size=0x1000 at link time is
 *     what keeps that true; ld's default max page size for x86-64 is 2 MiB and
 *     would pad the segments apart.
 *
 *   - The base address is 0x2000000 (32 MiB), matching RING3.EXE. It cannot be
 *     lower: the kernel image runs from a low identity alias reaching roughly
 *     _kernel_end = 0x1c00000, inflated by -g3, and create_vm_space() maps that
 *     alias into every process. Linking below it collides with the kernel's own
 *     PTEs.
 *
 *   - .rodata sits in the executable segment. That makes string literals
 *     executable, which is untidy but harmless here, and it keeps the number of
 *     PT_LOADs at two.
 *
 * Note the explicit /DISCARD/ list rather than the *(*) catch-all the older
 * hand-written test scripts use: a C binary has sections those scripts would
 * silently throw away, .rodata among them.
 */

ENTRY(_start)

PHDRS
{
  headers PT_PHDR PHDRS ;
  text    PT_LOAD FILEHDR PHDRS FLAGS(5) ;  /* R E */
  data    PT_LOAD FLAGS(6) ;                /* RW  */
}

SECTIONS
{
  . = 0x2000000 + SIZEOF_HEADERS;

  .text : {
    *(.text.startup)
    *(.text .text.*)
  } :text

  .rodata : {
    *(.rodata .rodata.*)
    *(.gnu.linkonce.r.*)
  } :text

  /* New segment starts here; keep vaddr and file offset congruent mod 4096. */
  . = ALIGN(0x1000);

  .data : {
    *(.data .data.*)
    *(.gnu.linkonce.d.*)
  } :data

  /* newlib's __libc_init_array walks these. Empty for plain C, but linking
   * them out would break anything using constructors. */
  .init_array : {
    __init_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    __init_array_end = .;
  } :data

  .fini_array : {
    __fini_array_start = .;
    KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
    KEEP(*(.fini_array))
    __fini_array_end = .;
  } :data

  .bss : {
    __bss_start = .;
    *(.bss .bss.*)
    *(COMMON)
    __bss_end = .;
  } :data

  . = ALIGN(0x1000);
  _end = .;
  PROVIDE(end = .);

  /DISCARD/ : {
    *(.comment)
    *(.note .note.*)
    *(.eh_frame)
    *(.eh_frame_hdr)
    *(.debug_gdb_scripts)
  }
}
