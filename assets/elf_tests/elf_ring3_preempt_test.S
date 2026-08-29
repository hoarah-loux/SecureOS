.intel_syntax noprefix
.global _start

# Ring3 preemption partner.
#
# elf_ring3_test.S and this one are meant to be loaded together as two separate
# processes. Neither yields, sleeps, or blocks: the only thing that can take
# the CPU away from one of them is IRQ0, so the two messages interleaving in
# the serial log is proof that timer_handler() is actually preempting. If
# preemption were broken, whichever one was scheduled first would print all of
# its rounds before the other printed any.
#
# Both then SYS_exit, which empties the ready queue and hands the CPU to the
# scheduler's idle thread.
#
# ABI is this OS's own int 0x80: nr in rax, args in rdi/rsi/rdx.
# See hdr/syscall.h. SYS_write == 1, SYS_exit == 7, fd 1 is the serial console.

.set SYS_write, 1
.set SYS_exit,  7
.set STDOUT,    1

# Rounds of work before exiting. The delay below is tuned so a round takes
# noticeably longer than one 50ms quantum (5 ticks at 100Hz), which is what
# makes the interleaving visible rather than a race.
.set ROUNDS,    8

.text
_start:
    mov r12, ROUNDS

round:
    # Pure busy-wait. Deliberately not a syscall or a hlt - this must be
    # uninterruptible-by-choice work so that losing the CPU can only be the
    # timer's doing.
    mov rcx, 0x4000000
delay:
    dec rcx
    jnz delay

    mov rax, SYS_write
    mov rdi, STDOUT
    lea rsi, [rip + msg]
    mov rdx, OFFSET msg_len
    int 0x80

    dec r12
    jnz round

    # SYS_exit(0). Does not return: the thread lands on the dying queue and
    # the scheduler switches to whatever else is runnable.
    mov rax, SYS_exit
    xor rdi, rdi
    int 0x80

    # Only reached if SYS_exit failed, which it does when the scheduler has
    # nothing to switch to. Spin rather than running off the mapped segment.
spin:
    jmp spin

msg:
    .ascii "[RING3-B] round\r\n"
    .set msg_len, . - msg
