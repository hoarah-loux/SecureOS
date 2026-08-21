.intel_syntax noprefix
.global _start

.text
_start:
    # SYS_write(fd=1, buf=msg, count=msg_len) via this OS's int 0x80 ABI
    # (see hdr/syscall.h: args in rdi, rsi, rdx, r10, r8, r9; SYS_write = 1).
    mov rax, 1
    mov rdi, 1
    lea rsi, [rip + msg]
    mov rdx, 18            # sizeof(msg) - GAS intel-syntax treats a bare
                            # "mov reg, symbol" as a memory load even for
                            # an absolute equate, so this is hardcoded.
    int 0x80

    # SYS_exit is still an unimplemented stub, so there's nothing to
    # correctly exit into yet - spin forever instead of falling off the
    # end of the mapped segment.
spin:
    jmp spin

msg:
    .ascii "hello from ring3\r\n"
