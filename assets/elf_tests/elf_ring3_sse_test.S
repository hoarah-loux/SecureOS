.intel_syntax noprefix
.global _start

# Ring3 SSE exerciser.
#
# The kernel is built -mno-sse and, until CR4.OSFXSR was set, never touched a
# vector register at all. That makes SSE the one piece of CPU state a ring3
# program can rely on that nothing in the kernel would notice losing: the other
# tests here would pass identically whether or not xmm registers survive a
# context switch, because none of them use one.
#
# This binary is what makes that visible. It is also the reason SSE has to work
# before any C program runs: the x86-64 SysV ABI passes floating-point
# arguments in xmm0-7 and sets al to the vector-register count for varargs, so
# a single printf("%f") depends on everything checked here.
#
# Steps, each reported by number:
#
#   1  SSE executes at all. Before CR0.EM was cleared and CR4.OSFXSR set, any
#      instruction below would have raised #UD and killed the process.
#   2  Integer SIMD round trip - movdqu out and back through memory, which is
#      also what gcc emits for an ordinary struct copy.
#   3  Floating-point arithmetic produces the right answer, so MXCSR came up
#      with sane rounding rather than as zeroes.
#   4  The whole xmm register file survives preemption. This is the real test:
#      xmm0-xmm7 are loaded with distinct values, then the program busy-waits
#      far longer than one 50ms quantum so IRQ0 is guaranteed to have taken the
#      CPU away and given it back several times, and only then are the
#      registers compared against what was put in them. Without the
#      fxsave/fxrstor pair in save_trap_frame()/restore_context this is what
#      fails, and it fails silently in a real program - as a wrong number, not
#      as a fault.
#
# ABI is this OS's own int 0x80: nr in rax, args in rdi/rsi/rdx.
# See hdr/syscall_abi.h. SYS_write == 1, SYS_exit == 7, fd 1 is serial.

.set SYS_write, 1
.set SYS_exit,  7
.set STDOUT,    1

# Long enough to cross several 10ms timer ticks with room to spare. The shell
# is on the ready queue too, so there is always something for the scheduler to
# switch to.
.set SPIN_COUNT, 0x8000000

# Per-build seed tag, overridden with `as --defsym SEED_TAG=n`.
#
# Two copies of this binary are built with different tags and run at the same
# time, which is what makes step 4 a real test. With only one SSE process on
# the system the step would pass whether or not the kernel saves anything:
# nothing else on the ready queue touches a vector register, since the kernel
# is built -mno-sse and the shell is kernel code. Two processes with different
# seeds clobber each other's xmm registers unless the context switch genuinely
# saves and restores them.
.ifndef SEED_TAG
.set SEED_TAG, 0
.endif

.text
_start:
    # ---- step 1: SSE instructions execute --------------------------------
    mov rbx, 1

    # xorps on a register with itself is the cheapest instruction that is
    # unambiguously SSE. Reaching the next instruction is the whole check.
    xorps xmm0, xmm0

    # ---- step 2: 128-bit integer round trip -------------------------------
    mov rbx, 2

    lea rsi, [rip + pattern_a]
    movdqu xmm1, [rsi]
    lea rdi, [rip + scratch]
    movdqu [rdi], xmm1

    mov rax, [rdi]
    mov rcx, [rsi]
    cmp rax, rcx
    jne fail
    mov rax, [rdi + 8]
    mov rcx, [rsi + 8]
    cmp rax, rcx
    jne fail

    # ---- step 3: double-precision arithmetic ------------------------------
    # 2.5 + 1.25 == 3.75, compared as an exact bit pattern. All three values
    # are exactly representable, so this is a real equality test and not an
    # epsilon comparison - a wrong MXCSR rounding mode would still produce the
    # right answer here, but a broken FPU init generally will not.
    mov rbx, 3

    lea rsi, [rip + val_2_5]
    movsd xmm2, [rsi]
    lea rsi, [rip + val_1_25]
    movsd xmm3, [rsi]
    addsd xmm2, xmm3

    lea rdi, [rip + scratch]
    movsd [rdi], xmm2
    mov rax, [rdi]
    lea rsi, [rip + val_3_75]
    mov rcx, [rsi]
    cmp rax, rcx
    jne fail

    # ---- step 4: xmm0-xmm7 survive preemption -----------------------------
    mov rbx, 4

    # Load eight distinct 128-bit patterns. Each is its index repeated, so a
    # register restored from the wrong slot is caught as well as one that was
    # not restored at all.
    lea rsi, [rip + reg_seed]
    movdqu xmm0, [rsi +   0]
    movdqu xmm1, [rsi +  16]
    movdqu xmm2, [rsi +  32]
    movdqu xmm3, [rsi +  48]
    movdqu xmm4, [rsi +  64]
    movdqu xmm5, [rsi +  80]
    movdqu xmm6, [rsi +  96]
    movdqu xmm7, [rsi + 112]

    # Busy-wait. Nothing here is a syscall or a hlt, so losing the CPU can
    # only be IRQ0's doing - the same reasoning as elf_ring3_preempt_test.S.
    # The registers above are untouched throughout, so anything that changes
    # them was the context switch.
    mov rcx, SPIN_COUNT
spin:
    dec rcx
    jnz spin

    # Spill all eight back to memory and compare against the seeds.
    lea rdi, [rip + spill]
    movdqu [rdi +   0], xmm0
    movdqu [rdi +  16], xmm1
    movdqu [rdi +  32], xmm2
    movdqu [rdi +  48], xmm3
    movdqu [rdi +  64], xmm4
    movdqu [rdi +  80], xmm5
    movdqu [rdi +  96], xmm6
    movdqu [rdi + 112], xmm7

    lea rsi, [rip + reg_seed]
    mov rcx, 16              # 8 registers x 2 qwords
    xor rdx, rdx
compare:
    mov rax, [rdi + rdx * 8]
    cmp rax, [rsi + rdx * 8]
    jne fail
    inc rdx
    dec rcx
    jnz compare

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


    # Both messages carry the seed tag, so a log with two of these in it shows
    # which copy reported what rather than two identical lines.
msg_ok:
    .ascii "ring3 sse ["
    .byte 0x30 + SEED_TAG
    .ascii "]: exec, movdqu, addsd, xmm0-7 across preempt all OK\r\n"
    .set msg_ok_len, . - msg_ok

msg_fail:
    .ascii "ring3 sse ["
    .byte 0x30 + SEED_TAG
    .ascii "]: FAILED at step X\r\n"
    .set msg_fail_len, . - msg_fail
    .set fail_digit_off, 30


.data
.align 16
pattern_a:
    .quad 0x0011223344556677
    .quad 0x8899AABBCCDDEEFF

val_2_5:
    .quad 0x4004000000000000   # 2.5
val_1_25:
    .quad 0x3FF4000000000000   # 1.25
val_3_75:
    .quad 0x400E000000000000   # 3.75

    # One 128-bit seed per register, each qword carrying the register index in
    # every byte so a cross-register mix-up is as visible as a lost value.
.align 16
reg_seed:
    .quad 0xA0A0A0A0A0A0A0A0 + SEED_TAG, 0xB0B0B0B0B0B0B0B0 + SEED_TAG   # xmm0
    .quad 0xA1A1A1A1A1A1A1A1 + SEED_TAG, 0xB1B1B1B1B1B1B1B1 + SEED_TAG   # xmm1
    .quad 0xA2A2A2A2A2A2A2A2 + SEED_TAG, 0xB2B2B2B2B2B2B2B2 + SEED_TAG   # xmm2
    .quad 0xA3A3A3A3A3A3A3A3 + SEED_TAG, 0xB3B3B3B3B3B3B3B3 + SEED_TAG   # xmm3
    .quad 0xA4A4A4A4A4A4A4A4 + SEED_TAG, 0xB4B4B4B4B4B4B4B4 + SEED_TAG   # xmm4
    .quad 0xA5A5A5A5A5A5A5A5 + SEED_TAG, 0xB5B5B5B5B5B5B5B5 + SEED_TAG   # xmm5
    .quad 0xA6A6A6A6A6A6A6A6 + SEED_TAG, 0xB6B6B6B6B6B6B6B6 + SEED_TAG   # xmm6
    .quad 0xA7A7A7A7A7A7A7A7 + SEED_TAG, 0xB7B7B7B7B7B7B7B7 + SEED_TAG   # xmm7


.bss
.align 16
scratch:
    .space 16
spill:
    .space 128
