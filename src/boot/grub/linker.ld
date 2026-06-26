/*
 * linker.ld — GNU ld linker script
 *
 * What this file is:
 * - A linker script controls how the linker (GNU ld) places sections in the
 *   final ELF (Executable and Linkable Format) kernel image.
 *
 * What this file guarantees for our boot path:
 * - The Multiboot2 header is placed early in the file so GRUB can find it.
 * - The kernel code (.text) is placed at a fixed virtual address (1 MiB).
 * - The resulting ELF program headers are split into two PT_LOAD segments:
 *     - RX (Read + eXecute) for code/read-only
 *     - RW (Read + Write) for writable data
 *   This removes the “RWX segment” warning and matches W^X intent.
 *
 * Abbreviations:
 * - ELF = Executable and Linkable Format.
 * - ld  = GNU linker.
 * - GRUB = GRand Unified Bootloader.
 * - PHDR / PHDRS = Program Header / Program Header definitions.
 * - PT_LOAD = a loadable segment type in ELF program headers.
 * - RX = Read + eXecute permissions.
 * - RW = Read + Write permissions.
 * - MiB = mebibyte (2^20 bytes).
 */

/* Declare the entry symbol in the final ELF. */
/* - `_start` is defined in the assembly entry stub. */
ENTRY(_start)

/*
 * ---------------- Program headers (PHDRS) ----------------
 *
 * Program headers (PHDRs) tell the loader (GRUB) what ranges of the file to
 * map into memory.
 *
 * Why we define PHDRS explicitly:
 * - Without PHDRS, the linker may create one PT_LOAD segment containing both
 *   code and writable data.
 * - That produces a PT_LOAD segment that is Read+Write+eXecute (RWX), which is
 *   undesirable and triggers toolchain warnings.
 *
 * What we want:
 * - One PT_LOAD segment that contains code and read-only data: RX.
 * - One PT_LOAD segment that contains writable data and runtime-only regions:
 *   RW.
 *
 * Important boundary:
 * - The PHDR flags influence the ELF metadata. Actual enforcement at runtime is
 *   done by page tables (e.g., NX bit, read-only pages).
 */
PHDRS
{
    /*
     * FLAGS() uses the ELF PF_* permission bits:
     * - PF_R = 4 (read)
     * - PF_W = 2 (write)
     * - PF_X = 1 (execute)
     */

    /*
     * `text` is a loadable segment with Read+eXecute permissions (RX).
     * - This is where we place the Multiboot2 header, .text, and .rodata.
     */
    text PT_LOAD FLAGS(5); /* 4 (R) + 1 (X) = 5 => RX */

    /*
     * `data` is a loadable segment with Read+Write permissions (RW).
     * - This is where we place .data, .bss, and NOLOAD runtime reservations.
     */
    data PT_LOAD FLAGS(6); /* 4 (R) + 2 (W) = 6 => RW */
}

SECTIONS
{
    /* Start placing sections at virtual address 1 MiB (0x0010_0000). */
    /* - Multiboot2 kernels are traditionally linked at 1 MiB. */
    /* - Our early paging is identity-mapped, so VA == PA initially. */
    . = 1M;

    /* Multiboot header MUST be in first 8 KiB (8192 bytes) of the file. */
    /*
     * Multiboot2 header:
     *   - Must appear early in the *file* so GRUB can find it.
     *   - It is not writable, and should be part of the RX segment.
     */
    .multiboot2_header : {
        /* Include the assembly-produced header section verbatim. */
        *(.multiboot2_header)
    } :text

    /*
     * Multiboot1 header (legacy/parallel path):
     * keep it loadable and non-writable as well.
     */
    .multiboot_header : {
        /* Keep the legacy Multiboot (version 1) header if present. */
        *(.multiboot_header)
    } :text

    /* Executable code section (RX). */
    .text : {
        /*
         * Symbols used by early paging code to apply NX safely.
         * NOTE: These are *virtual* addresses, but in our early identity-map
         * they equal physical addresses.
         */
        /* Mark the beginning of kernel code for NX bookkeeping. */
        __kernel_text_start = .;
        /* Pull in all .text and .text.* input sections from objects. */
        *(.text*)
        /* Mark the end of kernel code for NX bookkeeping. */
        __kernel_text_end = .;
    } :text

    /* Read-only data; kept in RX for simplicity and correctness. */
    .rodata : {
        /* Pull in all .rodata and .rodata.* input sections. */
        *(.rodata*)
    } :text

    /*
     * IMPORTANT: Segment boundary
     *
     * We deliberately start the RW segment AFTER the RX segment has finished,
     * on a page boundary. This avoids overlapping PT_LOAD segments in memory.
     *
     * Without this boundary, ld can end up creating overlapping segments when
     * a RW section is located “in the middle” of the RX address range.
     */
    /*
     * Security/sanity under early 2MiB paging:
     *   Our entry paging currently uses 2MiB pages.
     *   We clear NX for the 2MiB page(s) that overlap .text.
     *   If RW data lives in the same 2MiB page as .text, it becomes executable.
     *
     * So we start the RW segment on a 2MiB boundary to keep writable data in
     * a different 2MiB region from the (non-2MiB-aligned) .text at 1MiB.
     */
    /* Align the location counter to a 2 Mi B boundary (0x0020_0000). */
    /* - This ensures the RW segment starts on its own 2 Mi B region. */
    /* - It prevents .data/.bss/stack from sharing the same 2 Mi B page as .text
     *   under early 2 Mi B mappings.
     */
    . = ALIGN(0x200000);

    /* Writable initialized data (RW). */
    .data : {
        /* Pull in all .data and .data.* input sections. */
        *(.data*)
    } :data

    /* Writable zero-initialized data (RW). */
    .bss : {
        /* Expose BSS bounds so the entry stub can zero it. */
        __bss_start = .;
        /* Pull in all .bss and .bss.* input sections. */
        *(.bss*)
        /* End symbol for BSS clearing loop. */
        __bss_end = .;
    } :data

    /*
     * Early paging structures (RW, NOLOAD):
     *
     * Why it is a dedicated section:
     *   - Our entry stub needs physical memory to store page tables BEFORE we
     *     have a real allocator.
     *   - Hardcoding low addresses (like 0x1000) is unsafe because it can
     *     collide with GRUB’s Multiboot2 info structure or other allocations.
     *
     * Why it is placed here:
     *   - It must NOT be inside .bss (we zero .bss in the entry code).
     *   - It must be inside the RW PT_LOAD segment so the memory exists at
     *     runtime and we can write page table entries.
     */
    /* Align to a 4 KiB page boundary (4096 bytes). */
    /* - Page tables must be page-aligned. */
    . = ALIGN(4096);
    .pgtables (NOLOAD) : {
        /* Pull in the .pgtables input section (defined by the assembly stub). */
        *(.pgtables)
    } :data

    /*
     * Kernel stack reservation (RW, NOLOAD):
     *   - Like page tables, we need stack memory before we have an allocator.
     *   - NOLOAD keeps the file small; memory still exists at runtime.
     */
    .stack (NOLOAD) : {
        /* Reserve 32 KiB for the early kernel stack. */
        . += 32768;
        /* Export the top-of-stack symbol for the entry stub. */
        /* - Stack grows downward, so we start RSP at stack_top. */
        stack_top = .;
        /* Include any explicitly placed .stack input sections (if any). */
        *(.stack*)
    } :data

    /*
     * Early kernel heap reservation (RW, NOLOAD):
     *   - Fixed-size bootstrap heap inside the kernel image.
     *   - PMM will treat it as used because it is part of the image.
     *   - Later you can switch to a dynamic heap that grows on demand.
     */
    . = ALIGN(0x200000);
    .kheap (NOLOAD) : {
        __kheap_start = .;
        . += 0x01800000; /* 24 MiB */
        __kheap_end = .;
    } :data

    /* Mark the start and end of the kernel image for PMM reservation */
    _kernel_start = __kernel_text_start;
    _kernel_end = .;
}
