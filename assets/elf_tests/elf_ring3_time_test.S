.intel_syntax noprefix
.global _start

# Ring3 exerciser for SYS_brk, SYS_gettime and SYS_sleep.
#
# These three were stubs returning -1 until now, and all three sit directly
# under newlib: _sbrk() is SYS_brk (so malloc, stdio buffers and strdup are),
# _gettimeofday() is SYS_gettime, and doomgeneric's DG_SleepMs is SYS_sleep.
# Checking them from assembly first means a failure here is a kernel bug, not
# a libc retargeting bug - which is much harder to tell apart once newlib is
# in the picture.
#
# Steps:
#
#   1  brk(0) reports a break inside the reservation the loader made.
#   2  brk(new) grows it and returns the new value.
#   3  The memory the growth handed out is readable, writable and zero. This
#      is the demand-paging path: none of these pages existed until touched.
#   4  brk refuses to move outside the reservation, and reports the refusal by
#      returning the unchanged break rather than by failing outright.
#   5  gettime is monotonic - a second reading is not less than the first.
#   6  sleep actually sleeps. The clock must advance by at least the requested
#      interval across the call, which is what distinguishes a real block from
#      a stub that returns immediately.
#
# ABI is this OS's own int 0x80: nr in rax, args in rdi/rsi/rdx.
# See hdr/syscall_abi.h.

.set SYS_write,   1
.set SYS_exit,    7
.set SYS_brk,    15
.set SYS_gettime,16
.set SYS_sleep,  17
.set STDOUT,      1

# Must match USER_BRK_BASE / USER_BRK_SIZE in hdr/vmm.h.
.set BRK_BASE, 0x40000000
.set BRK_SIZE, 64 * 1024 * 1024

# How far to grow the break in step 2. Deliberately more than one page so the
# write loop in step 3 faults several pages in rather than just one.
.set GROW_BYTES, 3 * 4096

# Milliseconds to sleep in step 6. Comfortably more than one 10ms tick, so the
# check is not measuring rounding.
.set SLEEP_MS, 150

.text

# Writes "t<digit>\r\n" to the serial console, where the digit comes from bl.
# Called at the start of each step so a hang shows which step it hung in - the
# steps here can block (sleep) or fault (brk), and silence alone does not say
# which.
#
# Clobbers rax/rcx/rdx/rsi/rdi. rbx is preserved, since it carries the step
# number the failure path reports.
mark:
    push rbx
    mov rcx, rbx
    add cl, 0x30
    sub rsp, 16
    mov byte ptr [rsp], 't'
    mov [rsp + 1], cl
    mov byte ptr [rsp + 2], 0x0D
    mov byte ptr [rsp + 3], 0x0A
    mov rax, 1
    mov rdi, 1
    mov rsi, rsp
    mov rdx, 4
    int 0x80
    add rsp, 16
    pop rbx
    ret

_start:
    # ---- step 1: brk(0) reports the current break --------------------------
    mov rbx, 1
    call mark

    mov rax, SYS_brk
    xor rdi, rdi
    int 0x80

    # A stub would return -1 here, which is neither inside the reservation nor
    # a plausible address.
    mov r12, rax                    # r12 = original break, kept throughout

    mov rcx, BRK_BASE
    cmp r12, rcx
    jb fail
    mov rcx, BRK_BASE + BRK_SIZE
    cmp r12, rcx
    ja fail

    # ---- step 2: growing the break returns the new value -------------------
    mov rbx, 2
    call mark

    lea rdi, [r12 + GROW_BYTES]
    mov r13, rdi                    # r13 = requested new break
    mov rax, SYS_brk
    int 0x80

    cmp rax, r13
    jne fail

    # ---- step 3: the new memory is zero, and writable ----------------------
    # Every byte, not a sample: these pages arrive one #PF at a time and a
    # handler that mishandles one page in the middle is exactly the kind of
    # bug a spot check misses.
    mov rbx, 3
    call mark

    mov rsi, r12
    mov rcx, GROW_BYTES
zero_check:
    mov al, [rsi]
    test al, al
    jnz fail
    inc rsi
    dec rcx
    jnz zero_check

    # Write a pattern over the whole range, then read it back.
    mov rsi, r12
    mov rcx, GROW_BYTES
    mov al, 0x5A
write_fill:
    mov [rsi], al
    inc rsi
    dec rcx
    jnz write_fill

    mov rsi, r12
    mov rcx, GROW_BYTES
read_back:
    mov al, [rsi]
    cmp al, 0x5A
    jne fail
    inc rsi
    dec rcx
    jnz read_back

    # ---- step 4: brk refuses to leave the reservation ----------------------
    # Past the end of the arena. The break must not move, and the call must
    # report that by returning the value it already had.
    mov rbx, 4
    call mark

    mov rdi, BRK_BASE + BRK_SIZE + 4096
    mov rax, SYS_brk
    int 0x80

    cmp rax, r13
    jne fail

    # Below the base is equally out of bounds.
    mov rdi, BRK_BASE - 4096
    mov rax, SYS_brk
    int 0x80

    cmp rax, r13
    jne fail

    # ---- step 5: gettime is monotonic --------------------------------------
    mov rbx, 5
    call mark

    mov rax, SYS_gettime
    int 0x80
    mov r14, rax                    # r14 = first reading

    # A stub returning -1 fails here: -1 is negative, and the comparison below
    # is signed.
    test r14, r14
    js fail

    mov rcx, 0x400000
mono_delay:
    dec rcx
    jnz mono_delay

    mov rax, SYS_gettime
    int 0x80

    cmp rax, r14
    jl fail

    # ---- step 6: sleep advances the clock by at least the interval ---------
    mov rbx, 6
    call mark

    mov rax, SYS_gettime
    int 0x80
    mov r14, rax                    # r14 = before

    mov rax, SYS_sleep
    mov rdi, SLEEP_MS
    int 0x80

    # A stub returns -1; a working sleep returns 0.
    test rax, rax
    jnz fail

    mov rax, SYS_gettime
    int 0x80

    sub rax, r14
    cmp rax, SLEEP_MS
    jl fail

    # ---- all good ---------------------------------------------------------
    lea rsi, [rip + msg_ok]
    mov rdx, OFFSET msg_ok_len
    jmp report

fail:
    # rbx still holds the failing step number; turn it into an ASCII digit.
    add bl, 0x30

    # The template is in .text, which is mapped read-only, so patch a stack
    # copy rather than the original.
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

    mov rax, SYS_exit
    xor rdi, rdi
    int 0x80

spin_forever:
    jmp spin_forever


msg_ok:
    .ascii "ring3 time: brk grow/zero/bounds, gettime, sleep all OK\r\n"
    .set msg_ok_len, . - msg_ok

msg_fail:
    .ascii "ring3 time: FAILED at step X\r\n"
    .set msg_fail_len, . - msg_fail
    .set fail_digit_off, 27
