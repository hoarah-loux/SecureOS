.intel_syntax noprefix
.global _start

# Ring3 large-file loader test.
#
# Every other userland test binary here is under 4 KiB on disk, which means
# none of them ever exercises what happens when a file is bigger than a single
# page or a single FAT32 cluster:
#
#   - load_file() (loader.c) allocates the whole file with
#     kmalloc(file.entry.file_size), and kmalloc() refuses anything over 4096
#     (heap.c). Any binary past that size fails to load at all, which is
#     TODO.md's "load_file() will fail on any file over 4KB" item. This binary
#     is the reproducer for it, and the regression test once it is fixed.
#   - The FAT32 image is formatted with 512-byte clusters, so a 20 KiB payload
#     occupies ~40 clusters and cannot be served by reading one cluster or by
#     following a chain only a step or two. A wrong FAT chain walk, a repeated
#     cluster, or a short read shows up here as a specific wrong marker rather
#     than as a vague crash.
#
# What it checks, and why each check exists:
#
#   step 0..4  A distinct 8-byte marker at exactly 4096-byte stride, so each
#              one lands on a different page of the text segment and is served
#              by a different demand fault at a different file offset. The
#              value encodes its own index, so a page fetched from the wrong
#              offset (or the same page served twice) fails at a known index
#              instead of silently comparing equal.
#   step S     A qword deliberately straddling a page boundary: 4 bytes on one
#              page, 4 on the next. A loader that reads page-at-a-time but
#              misplaces a page by even a byte splits this value; nothing else
#              in this file would notice.
#   step T     The very last byte of the payload. The final cluster of the file
#              is a partial one, and a read that truncates to a whole number of
#              clusters or pages loses exactly this byte.
#
# The payload lives in its own .pages section, which the linker script places
# page-aligned inside the same read+exec PT_LOAD as .text. Alignment has to
# come from the script and not from .balign here: aligning inside .text would
# raise the whole output section's alignment and move it off the file-offset
# congruence that FILEHDR PHDRS in the same segment depends on.
#
# ABI is this OS's own int 0x80: nr in rax, args in rdi/rsi/rdx.
# See hdr/syscall.h. SYS_write == 1, SYS_exit == 7, fd 1 is the serial console.

.set SYS_write, 1
.set SYS_exit,  7
.set STDOUT,    1

# Number of 4 KiB-strided markers, and the base value they are built from.
# 0x4D41524B is "MARK" - a pattern that cannot appear by accident in zero-fill
# or in a page of ELF headers, so a bad read never coincidentally passes.
.set MARKER_COUNT, 5
.set MARKER_BASE,  0x4D41524B00000000

.text
_start:
    # ---- steps 0..4: one marker per page, walked by stride ----------------
    # r13 walks the markers, r14 is the index and doubles as the expected low
    # half of the value, so index and content are checked against each other.
    lea r13, [rip + marker0]
    xor r14, r14

marker_loop:
    mov rax, [r13]
    mov rcx, MARKER_BASE
    add rcx, r14
    cmp rax, rcx
    jne marker_fail

    add r13, 4096
    inc r14
    cmp r14, MARKER_COUNT
    jb marker_loop

    # ---- step S: the page-straddling qword ---------------------------------
    mov bl, 'S'
    lea rsi, [rip + straddler]
    mov rax, [rsi]
    mov rcx, 0x0A0B0C0D0E0F1011
    cmp rax, rcx
    jne fail

    # ---- step T: the last byte of the payload ------------------------------
    mov bl, 'T'
    lea rsi, [rip + tail_byte]
    movzx eax, byte ptr [rsi]
    cmp al, 0x5A
    jne fail

    # ---- all good ----------------------------------------------------------
    lea rsi, [rip + msg_ok]
    mov rdx, OFFSET msg_ok_len
    jmp report

marker_fail:
    # r14 holds the index of the marker that did not match; report it as a
    # digit. MARKER_COUNT is single digit by construction.
    mov rbx, r14
    add bl, 0x30

fail:
    # bl holds the step character. The message template lives in .text, which
    # is mapped read-only + execute, so patch a stack copy rather than the
    # original (same reasoning as elf_ring3_stack_test.S).
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

    mov [rsp + fail_char_off], bl
    mov rsi, rsp
    mov rdx, OFFSET msg_fail_len

report:
    mov rax, SYS_write
    mov rdi, STDOUT
    int 0x80

    # SYS_exit(0). Does not return: the kernel moves this thread to the dying
    # queue and switches to whatever else is runnable.
    mov rax, SYS_exit
    xor rdi, rdi
    int 0x80

    # Only reached if SYS_exit failed (it returns -1 when the scheduler has
    # nothing else to run). Spin rather than falling off the mapped segment.
spin:
    jmp spin

msg_ok:
    .ascii "ring3 big: 20 KiB payload, all markers + straddle + tail OK\r\n"
    .set msg_ok_len, . - msg_ok

msg_fail:
    .ascii "ring3 big: FAILED at step X\r\n"
    .set msg_fail_len, . - msg_fail
    # Offset of the 'X' placeholder within msg_fail, patched at runtime.
    .set fail_char_off, 26


# The payload. The linker script starts this section on a page boundary, so
# every .org below is an exact offset from a page-aligned base: marker N sits 8
# bytes into page N, and the straddler sits 4 bytes before the end of page 0.
# .org emits real bytes into the file (zero fill), which is what makes the
# binary genuinely 20 KiB on disk and not just in memory.
.section .pages, "a", @progbits
pages_start:

    .org 8
marker0:
    .quad MARKER_BASE + 0

    # Last 4 bytes of page 0 plus the first 4 of page 1.
    .org 4092
straddler:
    .quad 0x0A0B0C0D0E0F1011

    .org 4096 + 8
marker1:
    .quad MARKER_BASE + 1

    .org 8192 + 8
marker2:
    .quad MARKER_BASE + 2

    .org 12288 + 8
marker3:
    .quad MARKER_BASE + 3

    .org 16384 + 8
marker4:
    .quad MARKER_BASE + 4

    # Final byte of the payload, and of the file's last partial cluster.
    .org 20480 - 1
tail_byte:
    .byte 0x5A
