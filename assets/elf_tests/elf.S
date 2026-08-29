.intel_syntax noprefix
.global _start

.text
_start:
    # On x86_64, the exit system call is number 60.
    # We pass the status code in rdi.
    mov rax, 60         # sys_exit
    mov rdi, 42         # status code 42
    syscall
