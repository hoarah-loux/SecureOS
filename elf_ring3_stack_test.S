.intel_syntax noprefix
.global _start

# Ring3 stack exerciser.
#
# elf_ring3_test.S deliberately never touches its stack, so it cannot detect a
# broken user stack mapping - it runs identically whether or not the stack is
# usable. This binary is the opposite: every step below writes to the user
# stack, so it fails loudly if any part of that path regresses.
#
# What it covers:
#   1. A plain push/pop. This is the first write the process ever makes, so it
#      takes a demand page fault (error code 0x6, user + write + not-present)
#      and exercises page_fault_handler's user-write path end to end.
#   2. A call/ret with a real stack frame and locals, i.e. the compiler-shaped
#      access pattern rather than a bare push.
#   3. A `sub rsp` deep enough to walk off the first page onto a second one, so
#      more than one demand fault is needed and the region walk is exercised
#      past its first page.
#
# Each step compares what it read back against what it wrote, and the process
# prints exactly one line saying which step failed, or that all of them passed.
# Passing is a positive result, not just an absence of a panic.
#
# Historically this caught two bugs at once: a user stack top of
# 0xFFFF800000000000 (non-canonical on first push -> #GP), and intermediate
# page-table entries created without R/W (leaf PTE correct, directories above
# it read-only -> the demand-faulted page still refused the write).
#
# ABI is this OS's own int 0x80, not Linux: nr in rax, args in rdi/rsi/rdx.
# See hdr/syscall.h. SYS_write == 1, fd 1 is the serial console.

.set SYS_write, 1
.set STDOUT,    1

.text
_start:
    # ---- step 1: push/pop -------------------------------------------------
    # rbx holds the failure step number; 0 means everything passed so far.
    mov rbx, 1

    mov rax, 0x1122334455667788
    push rax
    pop rcx
    cmp rcx, rax
    jne fail

    # ---- step 2: call/ret with a frame and locals -------------------------
    mov rbx, 2

    mov rdi, 0x0BADC0DE0BADC0DE
    call frame_roundtrip          # returns the value it stored+reloaded
    mov rcx, 0x0BADC0DE0BADC0DE
    cmp rax, rcx
    jne fail

    # ---- step 3: cross a page boundary ------------------------------------
    # 8 KiB of frame guarantees at least one more 4 KiB page below whatever
    # the entry page was, so this cannot be serviced by the already-faulted
    # page and must demand-fault again.
    mov rbx, 3

    sub rsp, 8192
    mov rax, 0xFEEDFACECAFEBEEF
    mov [rsp], rax                # lowest byte of the new frame
    mov rdx, [rsp]
    add rsp, 8192
    cmp rdx, rax
    jne fail

    # ---- all good ---------------------------------------------------------
    lea rsi, [rip + msg_ok]
    mov rdx, OFFSET msg_ok_len
    jmp report

fail:
    # rbx still holds the failing step number; turn it into an ASCII digit.
    add bl, 0x30

    # Copy the template onto the stack before patching the digit into it. The
    # message lives in .text, which the loader maps read-only + execute, so
    # patching it in place would take a #PF (error 0x7, user write to a
    # present page) and we would lose the diagnostic we are trying to print.
    # Copying it also means the failure path exercises the stack too.
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


# uint64_t frame_roundtrip(uint64_t value)
#
# Builds a real frame, spills the argument into a local, reloads it, and
# returns it. Deliberately uses rbp-relative locals so the access pattern
# matches compiled code rather than a bare push/pop.
frame_roundtrip:
    push rbp
    mov rbp, rsp
    sub rsp, 32

    mov [rbp - 8], rdi
    mov [rbp - 16], rdi
    xor rax, rax                  # prove the reload is what we return
    mov rax, [rbp - 8]
    mov rcx, [rbp - 16]
    cmp rax, rcx
    jne frame_roundtrip_done      # mismatch: return whatever we read

frame_roundtrip_done:
    mov rsp, rbp
    pop rbp
    ret


msg_ok:
    .ascii "ring3 stack: push/pop, frame, page-cross all OK\r\n"
    .set msg_ok_len, . - msg_ok

msg_fail:
    .ascii "ring3 stack: FAILED at step X\r\n"
    .set msg_fail_len, . - msg_fail
    # Offset of the 'X' placeholder within msg_fail, patched at runtime.
    .set fail_digit_off, 28
