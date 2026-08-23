.intel_syntax noprefix
.global _start

# Ring3 demand-paging exerciser.
#
# elf_ring3_test.S and elf_ring3_stack_test.S both link to a single read+exec
# PT_LOAD that fits in one page and has no .bss, so neither can tell an eagerly
# populated segment from a demand-paged one, and neither touches the parts of
# the loader that only exist once segments are mmap()ed lazily.
#
# This binary is laid out specifically to hit those parts:
#
#   - Its text segment spans three pages, so running it faults in more than the
#     entry page and the file reads have to be at the right offsets, not just
#     the right first page.
#   - It has a real writable PT_LOAD with .data followed by 16 KiB of .bss, so
#     the segment splits into a file-backed region and an anonymous one.
#   - Its .data is small enough that the .bss start shares the final
#     file-backed page. Those trailing bytes must read as zero, not as whatever
#     follows in the ELF on disk - that is the single easiest thing to get
#     wrong when a partial page is filled from a file.
#
# Each step reports by number, so a failure says which property broke.
#
# ABI is this OS's own int 0x80: nr in rax, args in rdi/rsi/rdx.
# See hdr/syscall.h. SYS_write == 1, fd 1 is the serial console.

.set SYS_write, 1
.set STDOUT,    1

.text
_start:
    # ---- step 1: text content two pages further into the segment ----------
    # The entry page fault cannot have populated this; reading it forces at
    # least one more file-backed fault, at a non-zero page offset.
    mov rbx, 1

    lea rsi, [rip + far_marker]
    mov rax, [rsi]
    mov rcx, 0x1111222233334444
    cmp rax, rcx
    jne fail

    # ---- step 2: initialised .data came from the file ---------------------
    # Different PT_LOAD, different region, and a write-permitted one.
    mov rbx, 2

    lea rsi, [rip + data_val]
    mov rax, [rsi]
    mov rcx, 0x00BBCCDDEEFF0011
    cmp rax, rcx
    jne fail

    # ---- step 3: every byte of .bss reads as zero -------------------------
    # This is the whole point of the test. .bss starts inside the same page as
    # data_val, past p_filesz, and runs several pages past the end of the
    # file-backed region. Checking a single qword is not enough: whatever
    # follows the segment in the ELF (here the symbol table, whose first entry
    # is all zeroes) can happen to look like a correct zero fill. Scanning the
    # whole thing cannot be fooled that way, and it faults in every page of
    # both the file-backed and the anonymous region on the way through.
    mov rbx, 3

    lea rsi, [rip + bss_start]
    mov rcx, OFFSET bss_len
    xor rax, rax

bss_scan:
    or al, [rsi]
    inc rsi
    dec rcx
    jnz bss_scan

    test al, al
    jnz fail

    # ---- step 4: writes to a demand-faulted .bss page stick ---------------
    # Proves the region's write permission survived onto the lazily created
    # PTE, on a page that was faulted in by a read.
    mov rbx, 4

    lea rsi, [rip + bss_far]
    mov rcx, 0x0123456789ABCDEF
    mov [rsi], rcx
    mov rax, [rsi]
    cmp rax, rcx
    jne fail

    # ---- step 5: the write did not bleed into the next .bss page ----------
    mov rbx, 5

    lea rsi, [rip + bss_far2]
    mov rax, [rsi]
    test rax, rax
    jnz fail

    # ---- all good ---------------------------------------------------------
    lea rsi, [rip + msg_ok]
    mov rdx, OFFSET msg_ok_len
    jmp report

fail:
    # rbx still holds the failing step number; turn it into an ASCII digit.
    add bl, 0x30

    # The template lives in .text, which is mapped read-only + execute, so
    # patch a stack copy rather than the original (same reasoning as
    # elf_ring3_stack_test.S).
    sub rsp, 64
    lea rsi, [rip + msg_fail]
    mov rdi, rsp
    mov rcx, OFFSET msg_fail_len

fail_copy:
    mov al, [rsi]
    mov [rdi], al
    inc rsi
    inc rdi
    dec rcx
    jnz fail_copy

    mov [rsp + fail_digit_off], bl
    mov rsi, rsp
    mov rdx, OFFSET msg_fail_len

report:
    mov rax, SYS_write
    mov rdi, STDOUT
    int 0x80

    # SYS_exit is still a stub that returns -1, so there is nothing to exit
    # into - spin rather than running off the end of the mapped segment.
spin:
    jmp spin


msg_ok:
    .ascii "ring3 mmap: text pages, .data, .bss tail + anon all OK\r\n"
    .set msg_ok_len, . - msg_ok

msg_fail:
    .ascii "ring3 mmap: FAILED at step X\r\n"
    .set msg_fail_len, . - msg_fail
    # Offset of the 'X' placeholder within msg_fail, patched at runtime.
    .set fail_digit_off, 27

    # 8 KiB of padding pushes far_marker onto the third page of the text
    # segment. .space in .text emits real zero bytes into the file, which is
    # what makes this segment genuinely multi-page on disk as well as in
    # memory.
    .space 8192, 0
far_marker:
    .quad 0x1111222233334444


.data
    # Deliberately tiny: it has to leave the rest of its page to .bss so that
    # step 3 tests a partially file-backed page. The high byte is 0x00 rather
    # than 0xAA so a zeroed read cannot accidentally look like a short read of
    # something plausible.
data_val:
    .quad 0x00BBCCDDEEFF0011


.bss
    # First .bss bytes land in the same page as data_val, past p_filesz - that
    # partial page is what step 3 is really about.
bss_start:
    .space 8192
    # Far enough in to be served by the anonymous region, not the file one.
bss_far:
    .space 8
    .space 4088
    # A further page again, so step 5 can show step 4's write stayed put.
bss_far2:
    .space 8
    .set bss_len, . - bss_start
