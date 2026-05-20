; ============================================================
; NOTE: LEGACY / UNUSED FILE
; ------------------------------------------------------------
; This file is NOT used by the current build.
;
; Current boot path uses Multiboot2 + UEFI GRUB:
;   src/boot/grub/entry64_mb2.asm
;
; This legacy Multiboot1 experiment hard-codes page tables at 0x1000,
; which can clobber GRUB/Multiboot data, and its GDT setup is incomplete.
; Keep it only as a reference; do not enable it without re-auditing.
; ============================================================

; Abbreviations expanded (first use in this file):
; - GRUB = GRand Unified Bootloader
; - MB1  = Multiboot version 1
; - MB2  = Multiboot2
; - ELF  = Executable and Linkable Format
; - GDT  = Global Descriptor Table
; - IDT  = Interrupt Descriptor Table
; - PAE  = Physical Address Extension
; - MSR  = Model Specific Register
; - EFER = Extended Feature Enable Register (MSR 0xC0000080)
; - LME  = Long Mode Enable (EFER bit 8)
; - CR0/CR3/CR4 = Control Registers
; - PML4/PDPT/PD/PT = paging tables (4-level x86_64 paging)

; ============================================================
; entry64.asm
; ------------------------------------------------------------
; This is the VERY FIRST code executed after GRUB jumps
; into our kernel ELF.
;
; GRUB has ALREADY:
; - Loaded the kernel ELF into memory
; - Set CPU to 64-bit long mode
; - Enabled paging
; - Set flat segmentation
; - Set RIP = address of _start
;
; Our job here is ONLY:
; 1. Provide executable code (.text)
; 2. Set up a valid stack pointer (RSP)
; 3. Prepare the environment for C code
; 4. Jump into kmain()
; ============================================================

; Assemble the header/entry as 32-bit.
; - Multiboot version 1 header is defined for i386.
BITS 32

; Export the entry label.
GLOBAL _start

; External symbols resolved at link time.
EXTERN kmain
EXTERN stack_top
EXTERN __bss_start
EXTERN __bss_end

; ============================================================
; MULTIBOOT HEADER (GRUB Magic)
; ============================================================
; This header must:
; 1. Be in the first 8KB of the ELF file
; 2. Be 4-byte aligned
; 3. Contain the magic number 0x1BADB002
;
; GRUB scans for this header to recognize the kernel.
; ============================================================

; Place the Multiboot v1 header in its own section so the linker script can
; keep it near the start of the file.
SECTION .multiboot_header

; Multiboot v1 header requires 4-byte alignment.
ALIGN 4

multiboot_header:
    ; dd = 32-bit constant.
    ; Multiboot v1 magic constant.
    dd 0x1BADB002
    ; Flags.
    ; - 0 means “request no special MB1 features”.
    dd 0x00000000
    ; Checksum.
    ; - Required invariant:
    ;     magic + flags + checksum == 0 (mod 2^32)
    dd -(0x1BADB002 + 0x00000000)
                            ; Verification: 0x1BADB002 + 0x00000000 + 0xE4524FFB = 0x100000001 (wraps to 0)

; ============================================================
; SECTION: .text
; This is executable code.
; GRUB jumps directly here IN 32-BIT MODE.
; We must transition to 64-bit long mode.
; ============================================================

; Executable code section.
SECTION .text

; GRUB enters this legacy path in 32-bit protected mode.
BITS 32

_start:
    ; Disable interrupts.
    ; - No IDT is installed here; interrupts could triple-fault.
    cli
    
    ; Set up a 32-bit stack pointer.
    ; - This must be valid before any CALL/RET or push/pop heavy code.
    mov esp, stack_top
    
    ; Set up minimal paging structures for long mode.
    ; - This legacy code identity-maps only the first 2 MiB.
    
    ; Clear page tables.
    ; WARNING: This hard-codes 0x1000 as page-table memory.
    ; - This is unsafe in general because it can overwrite bootloader data.
    mov edi, 0x1000
    ; CR3 points at the top-level paging structure (PML4 physical address).
    mov cr3, edi
    ; Zero-fill the region.
    xor eax, eax
    ; ECX is dword count for `stosd`.
    mov ecx, 4096
    ; Store EAX into [EDI] repeatedly.
    rep stosd
    ; Reset EDI back to the base.
    mov edi, 0x1000
    
    ; PML4[0] -> PDPT.
    ; - 0x2000 is PDPT address.
    ; - 0x3 sets Present + Read/Write.
    mov dword [edi], 0x2003
    ; Move to the PDPT page.
    add edi, 0x1000
    
    ; PDPT[0] -> PD.
    ; - 0x3000 is PD address.
    ; - 0x3 sets Present + Read/Write.
    mov dword [edi], 0x3003
    ; Move to the PD page.
    add edi, 0x1000
    
    ; PD[0] -> 2 MiB page mapping.
    ; - 0x83 sets Present + Read/Write + PS (Page Size) for 2 MiB pages.
    mov dword [edi], 0x00000083
    
    ; Enable PAE (Physical Address Extension).
    mov eax, cr4
    ; CR4.PAE is bit 5.
    or eax, 1 << 5
    mov cr4, eax
    
    ; Enable long mode by setting EFER.LME.
    mov ecx, 0xC0000080
    rdmsr
    ; EFER.LME is bit 8.
    or eax, 1 << 8
    wrmsr
    
    ; Enable paging (CR0.PG).
    mov eax, cr0
    ; CR0.PG is bit 31.
    or eax, 1 << 31
    mov cr0, eax
    
    ; Load the GDT.
    lgdt [gdt64.pointer]
    
    ; Far jump to reload CS and enter 64-bit mode.
    jmp gdt64.code:long_mode_entry
    
; Assemble subsequent instructions as 64-bit.
BITS 64

long_mode_entry:

    ; ========================================================
    ; STACK SETUP
    ; ========================================================
    ; The CPU uses RSP implicitly for:
    ; - call / ret
    ; - interrupts
    ; - function prologues
    ;
    ; GRUB DOES NOT guarantee a valid stack.
    ; If we do not set this, the first CALL will crash.
    ;
    ; stack_top is defined in the linker script.
    ; It is just a memory address.
    ; ========================================================

    ; Install the 64-bit stack pointer.
    ; - Stack grows down, so we point at the top.
    mov rsp, stack_top

    ; Clear frame pointer.
    xor rbp, rbp

    ; ========================================================
    ; CPU STATE SANITY
    ; ========================================================

    ; Clear direction flag.
    cld

    ; ========================================================
    ; ZERO THE BSS SECTION
    ; ========================================================
    ; WHY WE ONLY ZERO BSS (and not other sections):
    ;
    ; .text (code)
    ;   - Contains executable instructions from entry64.asm and kernel code
    ;   - Already stored in the ELF file
    ;   - Already loaded correctly by GRUB
    ;   - No need to zero
    ;
    ; .data (initialized globals)
    ;   - Contains variables WITH explicit values (e.g., int count = 5;)
    ;   - The value 5 is stored in the ELF file
    ;   - GRUB loads it with the correct value already
    ;   - No need to zero
    ;
    ; .rodata (constants)
    ;   - Contains string literals and const values (e.g., const char* msg = "Hello";)
    ;   - Already in the ELF file with correct data
    ;   - GRUB loads it correctly
    ;   - No need to zero
    ;
    ; .bss (uninitialized)
    ;   - Contains variables WITHOUT values (e.g., int counter;)
    ;   - The ELF file does NOT store any bytes for this
    ;   - GRUB allocates memory but leaves it as garbage
    ;   - MUST zero it
    ;
    ; Bottom line: Only BSS is "empty" in the ELF file. Everything else
    ; already has correct data from the file, so there's nothing to zero.
    ;
    ; ========================================================
    ; The .bss section holds:
    ; - Uninitialized global variables
    ; - Static variables with no explicit value
    ;
    ; The ELF file describes BSS (with a section header) but does NOT
    ; store the actual data bytes (to save file size). GRUB allocates
    ; memory for it but leaves it uninitialized (garbage).
    ; We must manually zero it here.
    ;
    ; __bss_start and __bss_end are symbols
    ; defined by the linker script.
    ;
    ; WE ARE ZEROING THE BSS SECTION(SEGMENT) IN MEMORY HERE
    ; ========================================================

    ; Zero BSS.
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    ; Convert bytes -> 8-byte chunks.
    shr rcx, 3
    xor rax, rax
    rep stosq

    ; ========================================================
    ; ENTER THE KERNEL
    ; ========================================================
    ; At this point:
    ; - Stack is valid
    ; - CPU flags are sane
    ; - Global variables are zeroed
    ;
    ; We now jump into C.
    ; ========================================================

    ; Call the C kernel entry.
    call kmain

    ; ========================================================
    ; IF kmain RETURNS → HALT FOREVER
    ; ========================================================
    cli                       ; Disable interrupts
.hang:
    hlt                       ; Stop CPU until interrupt
    jmp .hang                 ; Infinite halt loop

; ============================================================
; 64-BIT GDT (Global Descriptor Table)
; ============================================================
SECTION .rodata
gdt64:
    dq 0                      ; Null descriptor
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53) ; Code segment
.pointer:
    dw $ - gdt64 - 1
    dq gdt64
