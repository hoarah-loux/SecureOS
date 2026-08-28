    ; Minimal Multiboot2 entry stub.
    ; most of the comments here are ai generated ONLY THE COMMENTS not all of them tho
    ; What this file is:
    ; - This NASM (Netwide Assembler) source file contains:
    ;   1) A Multiboot2 header (so GRUB recognizes the kernel)
    ;   2) The first instructions GRUB jumps to (entry point)
    ;   3) The CPU bring-up sequence into 64-bit long mode
    ;   4) Early paging setup (identity map) + safety hardening (NX + WP)
    ;   5) A call into the C kernel entry point: kmain(magic, info_ptr)
    ;
    ; Important boot-protocol fact:
    ; - Even when GRUB is an x86_64-efi (UEFI) binary, Multiboot2 does NOT
    ;   guarantee your kernel is entered in 64-bit long mode.
    ; - The Multiboot2 header architecture is i386 (32-bit), and the initial
    ;   entry state is treated as 32-bit protected mode for portability.
    ; - Therefore this stub sets up long mode itself.
    ;
    ; Abbreviations expanded (first use):
    ; - GRUB = GRand Unified Bootloader
    ; - UEFI = Unified Extensible Firmware Interface
    ; - MB2  = Multiboot2
    ; - ELF  = Executable and Linkable Format
    ; - ABI  = Application Binary Interface
    ; - SysV AMD64 ABI = System V x86-64 calling convention (Linux/Unix)
    ; - GDT  = Global Descriptor Table
    ; - PAE  = Physical Address Extension (required for IA-32e paging)
    ; - EFER = Extended Feature Enable Register (MSR 0xC0000080)
    ; - MSR  = Model Specific Register
    ; - LME  = Long Mode Enable (EFER bit 8)
    ; - LMA  = Long Mode Active (EFER bit 10)
    ; - NX   = No-eXecute (page permission bit controlled by EFER.NXE)
    ; - WP   = Write Protect (CR0.WP)
    ; - CR0/CR3/CR4 = Control Registers
    ; - PML4 = Page Map Level 4 table
    ; - PDPT = Page Directory Pointer Table
    ; - PD   = Page Directory
    ; - PT   = Page Table
    ; - PDE  = Page Directory Entry
    ; - PTE  = Page Table Entry
    ; - RX/RW/RWX = Read+Execute / Read+Write / Read+Write+Execute
    ; - COM1 = Serial port #1 (I/O port base 0x3F8)
    ; - VGA  = Video Graphics Array (legacy text buffer at 0xB8000)

    ; ============================================================
    ; entry64_mb2.asm
    ; ------------------------------------------------------------
    ; This is the VERY FIRST code executed after GRUB jumps
    ; into our kernel ELF.
    ;
    ; GRUB has loaded our ELF into memory.
    ; Our job here is:
    ; 1. Preserve MB2 magic/info_ptr
    ; 2. Build early page tables (identity-map low 4GiB)
    ; 3. Enable IA-32e paging + long mode + NX
    ; 4. Install a known-good GDT
    ; 5. Set up a valid stack + clear BSS
    ; 6. Call kmain(magic, info_ptr)
    ; ============================================================

    ; Tell NASM to assemble instructions as 32-bit.
    ; - The Multiboot2 header is defined for 32-bit architecture (i386).
    ; - GRUB enters the MB2 entry point in a 32-bit compatible state.
    BITS 32

    ; Export `_start` so the linker can mark it as the ELF entry point.
    GLOBAL _start
    GLOBAL tss64
    GLOBAL gdt64

    ; Symbols resolved from other compilation units / the linker script.
    ; - `kmain` is the C entry point we call after CPU setup.
    ; - `stack_top` is the top address of the reserved early stack.
    ; - `__bss_start`/`__bss_end` bound the BSS region to clear to zero.
    ; - `__kernel_text_start`/`__kernel_text_end` bound the executable code
    ;   region so we can selectively clear NX only for code pages.
    EXTERN kmain
    EXTERN stack_top
    EXTERN __bss_start
    EXTERN __bss_end
    EXTERN __kernel_text_start
    EXTERN __kernel_text_end

    ; ============================================================
    ; MULTIBOOT2 HEADER (GRUB Magic)
    ; ============================================================
    ; This header must:
    ; 1. Be in the first 8KB of the ELF file
    ; 2. Be 8-byte aligned
    ; 3. Contain the magic number 0xE85250D6
    ;
    ; GRUB scans for this header to recognize the kernel.
    ; ============================================================

    ; Place the MB2 header in its own section.
    ; - The linker script places this section early in the file (<8 KiB).
    SECTION .multiboot2_header

    ; Multiboot2 header must be 8-byte aligned.
    ALIGN 8

    ; Label marking the beginning of the MB2 header.
    multiboot2_header:
        ; dd = define 32-bit (double word) little-endian constant.
        ; MB2 magic constant required by the spec.
        dd 0xE85250D6

        ; Architecture field.
        ; - 0 means i386 (32-bit) for the Multiboot2 header format.
        dd 0x00000000

        ; Total header length in bytes.
        ; - Computed as end - start so it stays correct if tags change.
        dd multiboot2_header_end - multiboot2_header

        ; Checksum.
        ; - The MB2 spec requires:
        ;     magic + architecture + header_length + checksum == 0 (mod 2^32)
        ; - So we store the 2's-complement negative of the sum of the other
        ;   three fields.
        dd -(0xE85250D6 + 0x00000000 + (multiboot2_header_end - multiboot2_header))

        ; --------------------------------------------------------
        ; Tag: Entry address
        ; type = 3, flags = 0, size = 16 (must be 8-byte aligned)
        ; --------------------------------------------------------
        ; dw = define 16-bit (word) constant.
        ; Tag type 3: entry address tag.
        dw 0x0003
        ; Flags for this tag (0 = no special requirements).
        dw 0x0000
        ; Total size of this tag in bytes (must include header fields).
        dd 0x00000010
        ; The entry point GRUB should jump to.
        dd _start
        ; Padding so the next tag remains 8-byte aligned.
        dd 0x00000000

        ; --------------------------------------------------------
        ; Tag: Information request
        ; Ask for memory map information.
        ; - type 6  : classic BIOS E820-style memory map
        ; - type 17 : EFI memory map (commonly provided under UEFI)
        ; - type 9  : ELF sections (optional)
        ; - type 8  : framebuffer info (lets the kernel draw into the UEFI GOP framebuffer)
        ; type = 1, flags = 0, size = 24 (8-byte aligned)
        ; --------------------------------------------------------
        ; Tag type 1: information request.
        ; - This tells GRUB which boot-information tags we want in the MB2 info
        ;   structure passed to the kernel.
        dw 0x0001
        ; Flags (0 = optional; GRUB may omit unsupported tags).
        dw 0x0000
        ; Size of this tag.
        ; - 24 bytes means 8-byte header + 4 requested tag IDs.
        dd 0x00000018
        ; Request tag type 6: classic BIOS E820 memory map.
        dd 0x00000006
        ; Request tag type 17: EFI memory map.
        dd 0x00000011
        ; Request tag type 9: ELF sections information.
        dd 0x00000009
        ; Request tag type 8: framebuffer information.
        dd 0x00000008

        ; --------------------------------------------------------
        ; Tag: Framebuffer request
        ; Ask GRUB to set a graphics mode and keep a linear framebuffer.
        ; This makes the Multiboot2 info structure include tag type 8.
        ;
        ; type = 5, flags = 0, size = 24 (8-byte aligned)
        ; Fields:
        ;   width/height/depth: desired mode (GRUB may choose closest)
        ; --------------------------------------------------------
        ; Tag type 5: framebuffer request.
        ; - This asks GRUB to set up a graphics mode and provide a linear
        ;   framebuffer.
        dw 0x0005
        ; Flags (0 = no special requirements).
        dw 0x0000
        ; Size of this tag.
        ; - 24 bytes: header + width + height + depth + padding.
        dd 0x00000018
        ; Desired width in pixels (0 = any).
        dd 0
        ; Desired height in pixels (0 = any).
        dd 0
        ; Desired bits-per-pixel depth (0 = any).
        dd 0
        ; Padding to keep the overall header aligned.
        dd 0x00000000

        ; --------------------------------------------------------
        ; Tag: End (required)
        ; type = 0, flags = 0, size = 8
        ; --------------------------------------------------------
        ; Tag type 0: end tag.
        ; - Required terminator for the header tag list.
        dw 0x0000
        ; Flags.
        dw 0x0000
        ; Size (always 8 bytes for the end tag).
        dd 0x00000008

    multiboot2_header_end:

    ; ============================================================
    ; SECTION: .text
    ; Multiboot2 (arch = i386) hands control to a 32-bit entry point,
    ; even when GRUB itself is running as x86_64-efi.
    ;
    ; So we:
    ; - save EAX/EBX (magic/info_ptr)
    ; - switch to long mode
    ; - then run 64-bit setup + forward (magic, info_ptr) to C
    ; ============================================================

    ; ---------------- Early writable state ----------------
    ; Place these variables in writable data.
    ; - We use them to preserve MB2 registers across the 32->64 transition and
    ;   to report debug counts.
    SECTION .data

mb2_magic_saved: dd 0         ; Saved MB2 magic value (originally in EAX)
mb2_info_saved:  dd 0         ; Saved MB2 info pointer (originally in EBX)
nx_exec_2m_pages: dd 0        ; Count of 2 MiB pages with NX cleared (debug)
nx_exec_4k_pages: dd 0        ; Count of 4 KiB pages with NX cleared (debug)

    ; ---------------- Page table storage ----------------
    ; Place page tables in a dedicated section.
    ; - `nobits` means it occupies memory but has no file contents (like BSS).
    ; - The linker script maps this section into the RW segment but NOLOAD.
    ; - This prevents clobbering the MB2 info structure (often near 0x4000).
    SECTION .pgtables nobits

    ; Page tables must be 4 KiB aligned.
    ALIGN 4096

pgtables:
    ; 28KiB total:
    ;   +0x0000 PML4 (4KiB)
    ;   +0x1000 PDPT (4KiB)
    ;   +0x2000 PD0  (4KiB) -> maps 0..1GiB
    ;   +0x3000 PD1  (4KiB) -> maps 1..2GiB
    ;   +0x4000 PD2  (4KiB) -> maps 2..3GiB
    ;   +0x5000 PD3  (4KiB) -> maps 3..4GiB
    ;   +0x6000 PT0  (4KiB) -> maps 0..2MiB with 4KiB pages (for W^X / NX granularity)
    resb 0x7000

    ; Switch back to code section.


    ALIGN 16
tss64: 

    resb 0x68

tss64_end:
    
    SECTION .text

    ; The entry point is treated as 32-bit code.
    BITS 32

    _start:
        ; Disable maskable interrupts.
        ; - We do not have an Interrupt Descriptor Table (IDT) set up yet.
        ; - An interrupt here would almost certainly triple-fault.
        cli

        ; Clear the direction flag.
        ; - Required by the System V ABI convention (string ops increment).
        ; - Also prevents surprises if GRUB left DF set.
        cld

        ; Save Multiboot2-provided registers.
        ; - EAX contains the MB2 magic number (0x36D76289).
        ; - EBX contains the pointer to the MB2 information structure.
        ; - We store them because we will overwrite registers during setup.
        mov [mb2_magic_saved], eax
        mov [mb2_info_saved], ebx

        ; Install a temporary stack pointer in 32-bit mode.
        ; - `stack_top` is provided by the linker script.
        ; - We use ESP now; later in 64-bit we set RSP.
        mov esp, stack_top

        ; Build early page tables.
        ; - We identity-map 0..4 GiB (4 gibibytes) so that:
        ;   - low memory (including MB2 info at ~0x4000) is accessible
        ;   - the UEFI framebuffer physical address (often near 0x80000000 in
        ;     QEMU + OVMF) is accessible
        ; - Layout within `pgtables` (28 KiB total):
        ;   - +0x0000: PML4 (4 KiB)
        ;   - +0x1000: PDPT (4 KiB)
        ;   - +0x2000: PD0  (4 KiB) -> maps 0..1 GiB
        ;   - +0x3000: PD1  (4 KiB) -> maps 1..2 GiB
        ;   - +0x4000: PD2  (4 KiB) -> maps 2..3 GiB
        ;   - +0x5000: PD3  (4 KiB) -> maps 3..4 GiB
        ;   - +0x6000: PT0  (4 KiB) -> maps 0..2 MiB with 4 KiB pages
        ;
        ; EDI will point at the destination for `stosd`.
        mov edi, pgtables

        ; Zero EAX.
        ; - `rep stosd` stores EAX repeatedly; we want to clear the table area.
        xor eax, eax

        ; ECX = count of 32-bit stores.
        ; - 0x7000 bytes / 4 bytes-per-dword = 7168 dwords.
        mov ecx, 7168

        ; Clear the page-table backing memory.
        ; - Ensures all unused entries are zero (non-present).
        rep stosd

        ; Reset EDI to the PML4 base.
        mov edi, pgtables

        ; PML4[0] points to the PDPT.
        ; - We only populate entry 0 because we're identity-mapping low space.
        mov eax, pgtables + 0x1000
        ; Set Present (bit 0) and Read/Write (bit 1) => 0b11.
        or eax, 0x00000003
        ; Store low 32 bits of the 64-bit entry.
        mov dword [edi], eax
        ; Store high 32 bits of the 64-bit entry (0 here; address <4GiB).
        mov dword [edi + 4], 0

        ; Point EDI at the PDPT base.
        mov edi, pgtables + 0x1000

        ; PDPT[0..3] point to PD0..PD3.
        ; - Each PD (page directory) covers 1 GiB when using 2 MiB pages:
        ;     512 PDEs * 2 MiB = 1 GiB.
        ; - With four PDs, we cover 4 GiB total.

        ; Build PDPT[0] -> PD0.
        mov eax, pgtables + 0x2000
        ; Present + Read/Write flags.
        or eax, 0x00000003
        ; Store low 32 bits of PDPT entry 0.
        mov dword [edi + 0], eax
        ; Store high 32 bits (0 because address <4GiB).
        mov dword [edi + 4], 0

        ; Build PDPT[1] -> PD1.
        mov eax, pgtables + 0x3000
        or eax, 0x00000003
        mov dword [edi + 8], eax
        mov dword [edi + 12], 0

        ; Build PDPT[2] -> PD2.
        mov eax, pgtables + 0x4000
        or eax, 0x00000003
        mov dword [edi + 16], eax
        mov dword [edi + 20], 0

        ; Build PDPT[3] -> PD3.
        mov eax, pgtables + 0x5000
        or eax, 0x00000003
        mov dword [edi + 24], eax
        mov dword [edi + 28], 0

        ; PT0: 4KiB pages for first 2MiB.
        ; - 0..1MiB: RW + NX (includes MB2 info at 0x4000 and VGA at 0xB8000)
        ; - 1MiB..2MiB: RO; NX cleared only for pages overlapping kernel .text
        ; Point EDI at PT0 base (page table for 0..2 MiB).
        mov edi, pgtables + 0x6000

        ; EBX will hold the current physical base address for each 4 KiB page.
        ; - Start at 0.
        xor ebx, ebx

        ; ECX is the loop counter for 512 PTEs.
        ; - 512 * 4 KiB = 2 MiB.
        mov ecx, 512

        ; Reset the debug counter for executable 4 KiB pages.
        mov dword [nx_exec_4k_pages], 0
    .fill_pt0:
        ; Start building a PTE by using the physical page base address in EBX.
        mov eax, ebx

        ; Add Present + Read/Write flags by default.
        ; - Present (bit 0) = 1 means the mapping exists.
        ; - Read/Write (bit 1) = 1 means writable at CPL0.
        or eax, 0x00000003                 ; present + RW (default for <1MiB)

        ; If EBX < 1 MiB, keep it RW.
        cmp ebx, 0x00100000
        jb .pt0_rw_ok

        ; If EBX >= 1 MiB, clear the RW bit to make it read-only.
        ; - This is part of early W^X (Write xor Execute) hardening.
        and eax, 0xFFFFFFFD                ; clear RW (>= 1MiB)

    .pt0_rw_ok:
        ; Store low 32 bits of the PTE.
        mov dword [edi], eax

        ; Default: NX=1
        ; Default high dword: set NX bit.
        ; - For PTE/PDE entries, NX is bit 63.
        ; - In the high 32 bits, that's bit 31 (0x80000000).
        mov edx, 0x80000000

        ; Clear NX for pages overlapping kernel .text
        ; Load the executable .text bounds.
        ; - We will clear NX only for pages that overlap this range.
        mov esi, __kernel_text_start
        mov ebp, __kernel_text_end

        ; Compute end address of the current 4 KiB page: (base + 0x1000).
        mov eax, ebx
        add eax, 0x1000

        ; Overlap test with [__kernel_text_start, __kernel_text_end):
        ; - If page_end <= text_start: no overlap.
        cmp eax, esi
        jbe .pt0_store
        ; - If page_base >= text_end: no overlap.
        cmp ebx, ebp
        jae .pt0_store

        ; Overlaps .text: clear NX.
        xor edx, edx
        ; Increment debug count of executable 4 KiB pages.
        inc dword [nx_exec_4k_pages]
    .pt0_store:
        ; Store high 32 bits of the PTE (contains NX bit).
        mov dword [edi + 4], edx

        ; Advance physical base to next 4 KiB page.
        add ebx, 0x1000

        ; Advance EDI to the next PTE (each PTE is 8 bytes).
        add edi, 8

        ; Decrement ECX and repeat until all 512 entries are filled.
        loop .fill_pt0

        ; PD[0..2047] -> identity map first 4GiB.
        ; - First 2MiB uses PT0 (4KiB pages) for W^X / NX granularity.
        ; - Everything else uses 2MiB pages, NX by default.
        ; Each PDE is 64-bit. We use the high dword to set NX.
        ; Point EDI at PD0 base.
        ; - We fill PD0 with 512 entries, and then continue into PD1..PD3
        ;   because they are laid out contiguously in memory.
        mov edi, pgtables + 0x2000

        ; EBX will hold the current 2 MiB physical base address.
        xor ebx, ebx

        ; 2048 PDEs total maps 2048 * 2 MiB = 4 GiB.
        mov ecx, 2048

        ; Reset the debug counter for executable 2 MiB pages.
        mov dword [nx_exec_2m_pages], 0
    .fill_pd:
        ; Special-case the first 2 MiB region.
        ; - EBX == 0 indicates we're building PDE0.
        cmp ebx, 0
        jne .pde_2m

        ; PDE0 (0..2 MiB): point to PT0 (4 KiB pages), not a 2 MiB page.
        ; - PS (Page Size) bit must be 0 for “pointer to PT”.
        mov eax, pgtables + 0x6000
        ; Present + Read/Write.
        or eax, 0x00000003                 ; present + RW
        ; Store low 32 bits of PDE0.
        mov dword [edi], eax
        ; Store high 32 bits; ensure NX=0 at PDE level.
        mov dword [edi + 4], 0
        ; Jump to shared increment logic.
        jmp .pde_next

    .pde_2m:
        ; For all other regions, map using 2 MiB pages.
        mov eax, ebx
        ; Set flags:
        ; - Present (bit 0)
        ; - Read/Write (bit 1)
        ; - PS (Page Size, bit 7) = 1 indicates 2 MiB page mapping
        or eax, 0x00000083                 ; present + RW + PS
        ; Store low 32 bits of the PDE.
        mov dword [edi], eax

        ; NX policy: default NX=1, but clear NX for 2 MiB pages overlapping
        ; [__kernel_text_start, __kernel_text_end). The kernel links at
        ; 16 MiB now (see linker.ld) - outside PT0's 4 KiB-granular first
        ; 2 MiB - so without this the very first instruction after the jump
        ; to kmain would fault on an NX page. Overlap test:
        ;   page overlaps text iff base < text_end AND base+2MiB > text_start
        mov esi, __kernel_text_start
        mov ebp, __kernel_text_end
        cmp ebx, ebp
        jae .pde_2m_nx                     ; page base >= text end: no overlap
        lea edx, [ebx + 0x200000]
        cmp edx, esi
        jbe .pde_2m_nx                     ; page end <= text start: no overlap

        ; Overlaps kernel .text: executable (NX=0), count it for the debug
        ; report printed after long-mode entry.
        mov dword [edi + 4], 0
        inc dword [nx_exec_2m_pages]
        jmp .pde_next

    .pde_2m_nx:
        ; Store high 32 bits; set NX=1 by default.
        mov dword [edi + 4], 0x80000000    ; NX=1

    .pde_next:
        ; Advance physical base by 2 MiB.
        add ebx, 0x200000

        ; Advance to next PDE (each PDE is 8 bytes).
        add edi, 8

        ; Decrement ECX and loop until all 2048 entries are filled.
        loop .fill_pd

        ; Enable PAE (required for IA-32e paging)
        ; Enable PAE (Physical Address Extension) in CR4.
        ; - Required before enabling IA-32e paging (long mode paging).
        mov eax, cr4
        ; Set CR4.PAE (bit 5).
        or eax, 1 << 5
        mov cr4, eax

        ; Load CR3 with the PML4 base AFTER the tables are built.
        ; Load CR3 with the physical address of the PML4.
        ; - CR3 points to the top-level paging structure.
        mov edi, pgtables
        mov cr3, edi

        ; Enable long mode + NX support
        ; Enable long mode and NX in EFER.
        ; - EFER is MSR 0xC0000080.
        mov ecx, 0xC0000080
        ; Read EFER into EDX:EAX.
        rdmsr
        ; Set:
        ; - EFER.LME (Long Mode Enable, bit 8)
        ; - EFER.NXE (No-eXecute Enable, bit 11)
        or eax, (1 << 8) | (1 << 11)
        ; Write back EFER.
        wrmsr

        ; Enable paging + write-protect (CR0.WP) so supervisor writes to RO pages fault.
        ; Enable paging and write protection.
        ; - CR0.PG (Paging Enable, bit 31)
        ; - CR0.WP (Write Protect, bit 16)
        mov eax, cr0
        or eax, (1 << 31) | (1 << 16)
        mov cr0, eax

        ; Load 64-bit GDT and jump to long mode
        ; Load our 64-bit GDT (Global Descriptor Table).
        ; - Even in long mode, CS/SS selectors must reference valid descriptors.
        lgdt [gdt64.pointer]

        ; Far jump to reload CS and enter 64-bit code.
        ; - This completes the long mode transition.
        jmp gdt64.code:long_mode_entry

    ; Tell NASM that subsequent instructions are 64-bit.
    ; - After the far jump, the CPU is executing in 64-bit long mode.
    BITS 64

long_mode_entry:
    ; Install the real 64-bit stack pointer.
    ; - `stack_top` is the linker-defined address of our reserved stack.
    mov rsp, stack_top

    ; Align RSP to a 16-byte boundary.
    ; - The System V AMD64 ABI requires 16-byte stack alignment at CALL
    ;   boundaries for correct ABI compliance.
    and rsp, -16

    ; Load known-good data segments.
    ; In long mode, base/limit are mostly ignored for DS/ES/SS, but the
    ; selectors still must reference valid present descriptors.
    ; Load a known-good data segment selector into DS/ES/SS.
    ; - In long mode, segmentation base/limit are mostly ignored, but selectors
    ;   must still reference a valid present descriptor.
    mov ax, gdt64.data
    mov ds, ax
    mov es, ax
    mov ss, ax

   ; tss setup
    mov qword [rel tss64 + 4], rsp
    mov word  [rel tss64 + 0x66], 0x68

    lea rax, [rel tss64]
    mov word  [rel gdt64.tss + 2], ax
    shr rax, 16
    mov byte  [rel gdt64.tss + 4], al
    shr rax, 8
    mov byte  [rel gdt64.tss + 7], al
    shr rax, 8
    mov dword [rel gdt64.tss + 8], eax

mov ax, gdt64.tss
ltr ax

    ; Clear FS/GS selectors.
    ; - Many kernels later use FS/GS base MSRs for per-CPU/per-thread data.
    ; - For now, keep them null.
    xor ax, ax
    mov fs, ax
    mov gs, ax

        ; Clear the frame pointer.
        ; - Avoids confusing backtraces and satisfies common ABI expectations.
        xor rbp, rbp

        ; Clear the direction flag again for safety.
        ; - Some instructions (string ops) depend on DF.
        cld

        ; Restore the MB2 values we saved in 32-bit mode.
        ; - r12d will hold MB2 magic.
        ; - r13d will hold the MB2 info pointer.
        mov r12d, dword [rel mb2_magic_saved]
        mov r13d, dword [rel mb2_info_saved]

        ; Initialize serial output (COM1) for deterministic debug logging.
        ; - Under UEFI, VGA text is often not available.
        call serial_init

        ; Print an “entry reached” banner.
        ; - `lea` loads the address of the message into RSI.
        lea rsi, [rel msg_enter]
        call serial_write_string

        ; Verify the MB2 magic number.
        ; - If this does not match, something is wrong with the boot protocol.
        cmp r12d, 0x36D76289
        jne .bad_magic

        ; Verify long mode is active by checking EFER.LMA (Long Mode Active).
        ; - EFER.LMA is bit 10.
        mov ecx, 0xC0000080
        rdmsr
        test eax, 1 << 10
        jz .not_long

        ; Print confirmation message.
        lea rsi, [rel msg_long]
        call serial_write_string

        ; NX sanity: report how many pages are executable.
        ; With W^X enabled for the first 2MiB, .text should be executable via PT0.
        ; Print NX (No-eXecute) debug counts.
        ; - This helps verify that only intended pages are executable.
        lea rsi, [rel msg_nx_exec]
        call serial_write_string
        mov eax, dword [rel nx_exec_2m_pages]
        call serial_write_hex32
        lea rsi, [rel msg_crlf]
        call serial_write_string

        lea rsi, [rel msg_nx_exec4k]
        call serial_write_string
        mov eax, dword [rel nx_exec_4k_pages]
        call serial_write_hex32
        lea rsi, [rel msg_crlf]
        call serial_write_string

        ; Print the kernel .text bounds used for NX decisions.
        lea rsi, [rel msg_nx_text_start]
        call serial_write_string
        mov eax, __kernel_text_start
        call serial_write_hex32
        lea rsi, [rel msg_crlf]
        call serial_write_string

        lea rsi, [rel msg_nx_text_end]
        call serial_write_string
        mov eax, __kernel_text_end
        call serial_write_hex32
        lea rsi, [rel msg_crlf]
        call serial_write_string

        ; Debug: print the MB2 info pointer (physical address).
        lea rsi, [rel msg_info_ptr]
        call serial_write_string
        mov eax, r13d
        call serial_write_hex32
        lea rsi, [rel msg_crlf]
        call serial_write_string

        ; Read MB2 info total_size field.
        ; - total_size is the first 32-bit field in the MB2 info structure.
        mov eax, dword [r13 + 0]
        lea rsi, [rel msg_total_size]
        call serial_write_string
        call serial_write_hex32
        lea rsi, [rel msg_crlf]
        call serial_write_string

        ; Read the type of the first MB2 tag.
        ; - Tags start at offset 8 (after total_size + reserved).
        mov eax, dword [r13 + 8]
        lea rsi, [rel msg_first_tag]
        call serial_write_string
        call serial_write_hex32
        lea rsi, [rel msg_crlf]
        call serial_write_string

        ; Confirm the MB2 info structure contains a memory map tag.
        ; - We accept either type 6 (E820) or type 17 (EFI memory map).
        mov rdi, r13
        call mb2_find_memmap_tag
        cmp eax, 6
        je .mmap_e820
        cmp eax, 17
        je .mmap_efi
        jmp .no_mmap

    .mmap_e820:
        ; Report E820 map is present.
        lea rsi, [rel msg_mmap_e820]
        call serial_write_string
        jmp .after_debug

    .mmap_efi:
        ; Report EFI memory map is present.
        lea rsi, [rel msg_mmap_efi]
        call serial_write_string
        jmp .after_debug

    .bad_magic:
        ; MB2 magic mismatch.
        lea rsi, [rel msg_bad_magic]
        call serial_write_string
        jmp .hang

    .not_long:
        ; EFER.LMA was not set.
        lea rsi, [rel msg_not_long]
        call serial_write_string
        jmp .hang

    .no_mmap:
        ; Neither E820 nor EFI memory map tags were found.
        lea rsi, [rel msg_mmap_no]
        call serial_write_string

    .after_debug:

        ; Zero the BSS region.
        ; - BSS holds “uninitialized” globals; ELF does not store its bytes.
        mov rdi, __bss_start
        mov rcx, __bss_end
        ; RCX = length in bytes.
        sub rcx, rdi
        ; Value to store (AL=0).
        xor eax, eax
        ; Store zero bytes repeatedly.
        rep stosb

        ; Pass (magic, info_ptr) to kmain for later parsing.
        ; SysV AMD64 ABI: rdi, rsi are first two integer args.
        ; Call kmain(magic, info_ptr).
        ; - SysV AMD64 ABI argument registers:
        ;   - RDI = 1st integer/pointer argument
        ;   - RSI = 2nd integer/pointer argument
        mov edi, r12d
        mov esi, r13d
        call kmain

    .hang:
        ; If we ever reach here, stop forever.
        cli
        hlt
        jmp .hang


; ============================================================
; Serial output (COM1 @ 0x3F8)
; ============================================================

; Define a constant for the base I/O port of COM1.
; - COM1 = serial port #1.
; - 0x3F8 is the legacy PC UART base port for COM1.
%define COM1 0x3F8

serial_init:
    ; Initialize the UART for serial output.
    ; Target configuration:
    ; - 38400 baud (with divisor 3 assuming 115200 base)
    ; - 8 data bits, no parity, 1 stop bit (8N1)
    ; - FIFO enabled

    ; Select Interrupt Enable Register (IER) at COM1+1.
    mov dx, COM1 + 1
    ; AL = 0.
    xor al, al
    ; Disable all UART interrupts.
    out dx, al

        ; Select Line Control Register (LCR) at COM1+3.
        mov dx, COM1 + 3
        ; Set DLAB (Divisor Latch Access Bit) so COM1+0/1 access divisor.
        mov al, 0x80
        out dx, al

        ; Divisor Latch Low byte at COM1+0.
        mov dx, COM1 + 0
        ; Divisor = 3 => 115200 / 3 = 38400 baud.
        mov al, 0x03
        out dx, al

        ; Divisor Latch High byte at COM1+1.
        mov dx, COM1 + 1
        ; High byte = 0.
        xor al, al
        out dx, al

        ; Restore LCR to 8N1 with DLAB cleared.
        ; - 0x03 => 8-bit word length, 1 stop bit, no parity.
        mov dx, COM1 + 3
        mov al, 0x03
        out dx, al

        ; FIFO Control Register (FCR) at COM1+2.
        ; - 0xC7 enables FIFO, clears RX/TX queues, sets 14-byte threshold.
        mov dx, COM1 + 2
        mov al, 0xC7
        out dx, al

        ; Modem Control Register (MCR) at COM1+4.
        ; - 0x0B sets DTR/RTS and OUT2 (often required to enable interrupts,
        ;   though we keep interrupts disabled; still a common “known-good” init).
        mov dx, COM1 + 4
        mov al, 0x0B
        out dx, al
        ; Return to caller.
        ret

serial_write_char:
    ; Write one character to the serial port.
    ; Calling convention used here:
    ; - We pass the character in DIL (low 8 bits of RDI) in callers.
    ; - This function polls the UART until the transmit-holding register is empty.
    .wait:
    ; Line Status Register (LSR) at COM1+5.
    mov dx, COM1 + 5
    ; Read LSR.
    in al, dx
    ; Test THRE (Transmit Holding Register Empty) bit (bit 5 = 0x20).
    test al, 0x20
    ; If not empty, keep waiting.
    jz .wait
    ; Data register at COM1+0.
    mov dx, COM1
    ; Load the character to send.
    mov al, dil
    ; Transmit the byte.
    out dx, al
    ; Return to caller.
    ret

serial_write_string:
        ; Write a NUL-terminated string to serial.
        ; - RSI points to the first byte of the string.
        ; - The string must be terminated by a 0 byte.
    .next:
        ; Load next byte from [RSI] into AL and increment RSI.
        lodsb
        ; Test if AL == 0 (NUL terminator).
        test al, al
        ; If end, return.
        jz .done
        ; Move character into DIL for serial_write_char.
        mov dil, al
        ; Output the character.
        call serial_write_char
        ; Continue loop.
        jmp .next
    .done:
        ; Return to caller.
        ret

serial_write_hex32:
        ; Print a 32-bit value in hexadecimal.
        ; - Input: EAX = value.
        ; - Output: 8 ASCII hex digits (most-significant nibble first).
        ; Save caller-saved registers we use.
        push rbx
        push rcx
        push rdx
        ; Copy the input so we can shift it.
        mov ebx, eax
        ; Loop count: 8 nibbles.
        mov ecx, 8
    .hex_loop:
        ; Move current value into EAX.
        mov eax, ebx
        ; Bring the top nibble to the low bits.
        shr eax, 28
        ; Mask to 0..15.
        and al, 0x0F
        ; If <= 9, emit '0'..'9'.
        cmp al, 9
        jbe .digit
        ; Else emit 'A'..'F'.
        add al, 'A' - 10
        jmp .out
    .digit:
        ; Convert 0..9 to ASCII digit.
        add al, '0'
    .out:
        ; Output this ASCII character.
        mov dil, al
        call serial_write_char
        ; Shift next nibble into place.
        shl ebx, 4
        ; Repeat for all nibbles.
        loop .hex_loop
        ; Restore registers.
        pop rdx
        pop rcx
        pop rbx
        ; Return.
        ret


; ============================================================
; Multiboot2 tag scan helpers
; ============================================================

mb2_find_memmap_tag:
    ; Scan the MB2 info structure for a memory map tag.
    ; Inputs:
    ; - RDI = pointer to MB2 info structure (physical/identity-mapped address)
    ; Output:
    ; - EAX = 6  if tag type 6 (E820 memory map) is found
    ; - EAX = 17 if tag type 17 (EFI memory map) is found
    ; - EAX = 0  if neither is found or if the structure is invalid
    ;
    ; Read total_size (first 32-bit field at offset 0).
    mov edx, dword [rdi + 0]
    ; Sanity-check: minimum MB2 info size.
    ; - Must include total_size + reserved (8 bytes) and at least an end tag.
    cmp edx, 16
    jb .no
    ; RCX = end pointer = base + total_size.
    lea rcx, [rdi + rdx]
    ; RSI = first tag pointer (base + 8).
    lea rsi, [rdi + 8]
    .scan:
    ; Ensure we can read at least the 8-byte tag header.
    lea r8, [rsi + 8]
    cmp r8, rcx
    ja .no

        ; Read tag type.
        mov eax, dword [rsi + 0]
        ; Type 0 is the end tag.
        test eax, eax
        jz .no
        ; Check for E820 memory map tag.
        cmp eax, 6
        je .ret
        ; Check for EFI memory map tag.
        cmp eax, 17
        je .ret
        ; Read tag size in bytes.
        mov edx, dword [rsi + 4]
        ; Size must be at least 8 bytes (header size).
        cmp edx, 8
        jb .no
        ; Advance RSI by tag size.
        add rsi, rdx
        ; Round up to next 8-byte boundary (MB2 tags are 8-byte aligned).
        add rsi, 7
        and rsi, -8
        ; Bounds check: do not run off the end.
        cmp rsi, rcx
        ja .no
        ; Continue scanning.
        jmp .scan
    .ret:
        ret
    .no:
        ; Not found / invalid.
        xor eax, eax
        ret


; ---------------- Global Descriptor Table (GDT) ----------------
; Place the GDT in writable data.
; - Some CPUs set the “accessed” bit in descriptors, which is a write.
; - If the GDT were in read-only memory under CR0.WP, this could fault.
SECTION .data

    ; Align GDT on an 8-byte boundary.
    ALIGN 8

gdt64:
    ; Null descriptor (required): selector 0 is invalid.
    dq 0
.code: equ $ - gdt64
    ; 64-bit code segment descriptor: base=0, limit=max, present, ring0,
    ; executable + readable, long-mode (L=1), default operand size (D=0).
    ; Descriptor bits (high level):
    ; - P=1 (present)
    ; - DPL=0 (ring 0)
    ; - S=1 (code/data)
    ; - Type=0xA (execute/read code)
    ; - L=1 (64-bit)
    ; - D=0 (no 32-bit default)
    dq 0x00AF9A000000FFFF
.data: equ $ - gdt64
    ; Data segment descriptor (used for DS/ES/SS): base=0, limit=max, present,
    ; ring0, writable, 32-bit default size (D=1), L=0.
    ; Descriptor bits (high level):
    ; - P=1 (present)
    ; - DPL=0 (ring 0)
    ; - S=1 (code/data)
    ; - Type=0x2 (read/write data)
    ; - L=0 (not a code segment)
    ; - D=1 (default operand size; common for data)
    dq 0x00CF92000000FFFF


.ucode: equ $ - gdt64

    ;user code segment selector
    dq 0x00AFFA000000FFFF

.udata: equ $ - gdt64


    ;user data segment selector

    dq 0x00CFF2000000FFFF

.tss: equ $ - gdt64

    dw (tss64_end - tss64 - 1)                 ; limit 15:0
    dw 0                                       ; base 15:0 (patched at runtime)
    db 0                                       ; base 23:16 (patched at runtime)
    db 0x89                                    ; type=9, S=0, DPL=0, P=1
    db (((tss64_end - tss64 - 1) >> 16) & 0x0F) ; flags=0, limit 19:16
    db 0                                       ; base 31:24 (patched at runtime)
    dd 0                                       ; base 63:32 (patched at runtime)
    dd 0                                       ; reserved

.pointer:
    ; GDTR (GDT Register) pseudo-descriptor used by LGDT.
    ; - 16-bit limit = size of GDT in bytes minus 1.
    dw $ - gdt64 - 1
    ; - 64-bit base address of the GDT.
    dq gdt64

SECTION .rodata

; ---------------- Read-only message strings ----------------
; `db` defines raw bytes in memory (ASCII characters here).
; - 13 = carriage return ('\r')
; - 10 = line feed ('\n')
; - 0  = NUL terminator for C-style strings

msg_enter:     db "[SecureOS] MB2 entry reached", 13, 10, 0
msg_long:      db "[SecureOS] Long mode active (EFER.LMA=1)", 13, 10, 0
msg_nx_exec:   db "[SecureOS] NX: exec 2MiB pages (NX cleared)=0x", 0
msg_nx_exec4k: db "[SecureOS] NX: exec 4KiB pages (NX cleared)=0x", 0
msg_nx_text_start: db "[SecureOS] NX: __kernel_text_start=0x", 0
msg_nx_text_end:   db "[SecureOS] NX: __kernel_text_end=0x", 0
msg_info_ptr:  db "[SecureOS] mb2_info_ptr=0x", 0
msg_total_size: db "[SecureOS] mb2_total_size=0x", 0
msg_first_tag: db "[SecureOS] mb2_first_tag_type=0x", 0
msg_crlf:      db 13, 10, 0
msg_bad_magic: db "[SecureOS] ERROR: Bad MB2 magic", 13, 10, 0
msg_not_long:  db "[SecureOS] ERROR: Not in long mode", 13, 10, 0
msg_mmap_e820: db "[SecureOS] MB2 E820 mmap tag (type 6) present", 13, 10, 0
msg_mmap_efi:  db "[SecureOS] MB2 EFI memory map tag (type 17) present", 13, 10, 0
msg_mmap_no:   db "[SecureOS] ERROR: No MB2 memory map tag (type 6/17) found", 13, 10, 0

