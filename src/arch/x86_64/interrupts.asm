global page_fault_isr
extern page_fault_handler

SECTION .text

page_fault_isr:




push rax
push rbx
push rcx
push rdx
push rsi 
push rdi
push rbp
push r8
push r9
push r10
push r11
push r12
push r13
push r14
push r15

;;faulting va
mov rdi ,cr2
;;error code
mov rsi ,[rsp + 15*8]
;;rip
mov rdx, [rsp + 15*8 + 8]

mov r12, rsp
and rsp, -16



call page_fault_handler

mov rsp, r12


pop r15
pop r14
pop r13
pop r12
pop r11
pop r10
pop r9
pop r8
pop rbp
pop rdi
pop rsi
pop rdx
pop rcx
pop rbx
pop rax

add rsp, 8

iretq
