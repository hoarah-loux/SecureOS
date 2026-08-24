.intel_syntax noprefix
.global _start

# Ring3 loader smoke test, and the A side of the preemption test.
#
# Loaded alongside elf_ring3_preempt_test.S (RING3P.EXE) as a separate process.
# Neither yields or blocks, so interleaved [RING3-A]/[RING3-B] output is proof
# that the timer is preempting between them. See elf_ring3_preempt_test.S.
#
# ABI is this OS's own int 0x80: nr in rax, args in rdi/rsi/rdx.
# See hdr/syscall.h. SYS_write == 1, SYS_exit == 7, fd 1 is the serial console.

.set SYS_write, 1
.set SYS_exit,  7
.set STDOUT,    1

.set ROUNDS,    8

.text
_start:
    mov r12, ROUNDS

round:
    # Busy-wait rather than a syscall, so the only way to lose the CPU here is
    # IRQ0. Same constant as the B side, so neither gets a head start.
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

    # SYS_exit(0). Does not return: the kernel moves this thread to the dying
    # queue and switches to whatever else is runnable.
    mov rax, SYS_exit
    xor rdi, rdi
    int 0x80

    # Only reached if SYS_exit failed (it returns -1 when the scheduler has
    # nothing else to run). Spin rather than falling off the mapped segment.
spin:
    jmp spin

msg:
    .ascii "[RING3-A] round\r\n"
    .set msg_len, . - msg
