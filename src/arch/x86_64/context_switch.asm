BITS 64

SECTION .text

global restore_context
extern tss64

; Offsets into `context` from hdr/context.h.
; The leading bool occupies one byte, followed by seven bytes of padding.
%define CTX_RAX            8
%define CTX_RBX           16
%define CTX_RCX           24
%define CTX_RDX           32
%define CTX_RSI           40
%define CTX_RDI           48
%define CTX_RBP           56
%define CTX_R8            64
%define CTX_R9            72
%define CTX_R10           80
%define CTX_R11           88
%define CTX_R12           96
%define CTX_R13          104
%define CTX_R14          112
%define CTX_R15          120
%define CTX_RIP          128
;;this rsp is for the current execution
%define CTX_KERNEL_RSP   136 
%define CTX_RFLAGS       144
%define CTX_CR3          152
%define CTX_USER_RSP     160
%define CTX_CS           168
%define CTX_SS           176
%define CTX_OWNER_THREAD 184

; Offset into `thread` from hdr/t.h. this rsp is fro the stack top
%define THREAD_RSP_K      80

; void restore_context(context *next);
;
; SysV AMD64 entry contract:
;   RDI = next
;
; This function never returns. It constructs the same saved-register layout
; used by timer_isr, switches to the selected address space, restores every
; general-purpose register, and resumes the selected thread with IRETQ.
restore_context:
    ; Everything below is one critical section. Between the `mov rsp` that
    ; switches to the destination thread's kernel stack and the final IRETQ,
    ; RSP belongs to a thread that is not running yet and the IRETQ frame is
    ; only half built - and this runs at CPL0, so an interrupt taken here does
    ; not switch stacks, it lands right on top of that half-built frame.
    ; timer_handler() would then save_trap_frame() a RIP pointing into the
    ; middle of this function as the destination thread's resume point and
    ; reschedule into it, which comes back as a #GP on garbage popped by the
    ; IRETQ below.
    ;
    ; The ISR paths reach here with IF already clear (interrupt gates), so this
    ; only matters for calls made from ordinary kernel code, like kmain's
    ; handoff to the first thread. IRETQ restores RFLAGS from the context, so
    ; IF comes back on its own at the switch.
    cli

    test rdi, rdi
    jz .invalid_context

    ; Keep the context pointer in R11 while the destination frame is built.
    ; Its saved value is restored later from the constructed frame.
    mov r11, rdi

    ; Point this CPU's ring-0 privilege-entry stack at the selected thread's
    ; kernel-stack top. The GDT descriptor and task register remain unchanged;
    ; only the live TSS.RSP0 field is updated.
    mov rax, [r11 + CTX_OWNER_THREAD]
    test rax, rax
    jz .invalid_context

    mov rdx, [rax + THREAD_RSP_K]
    test rdx, rdx
    jz .invalid_context

    mov qword [rel tss64 + 4], rdx

    ; Both return paths construct their IRETQ frame on the selected thread's
    ; kernel stack. For a saved CPL0 context this is the interrupted kernel RSP;
    ; for a CPL3 context it is the thread's kernel-stack top.
    mov rsp, [r11 + CTX_KERNEL_RSP]
    test rsp, rsp
    jz .invalid_context

    ; IRETQ decides whether to consume three or five words from the target
    ; CS privilege level, so use CS.RPL as the authoritative frame selector.
    mov rax, [r11 + CTX_CS]
    and eax, 3
    cmp eax, 0
    je .build_kernel_iret_frame
    cmp eax, 3
    je .build_user_iret_frame
    jmp .invalid_context

.build_user_iret_frame:
    ; A CPL3 return consumes five words. Push them in reverse consumption order
    ; so RIP is at the top of the IRETQ frame.
    push qword [r11 + CTX_SS]
    push qword [r11 + CTX_USER_RSP]
    push qword [r11 + CTX_RFLAGS]
    push qword [r11 + CTX_CS]
    push qword [r11 + CTX_RIP]
    jmp .build_gpr_frame

.build_kernel_iret_frame:
    ; A same-privilege CPL0 return consumes only RIP, CS, and RFLAGS. (fuckass chatgpt (it was wrong))
    push qword [r11 + CTX_SS]
    push qword [r11 + CTX_KERNEL_RSP]
    push qword [r11 + CTX_RFLAGS]
    push qword [r11 + CTX_CS]
    push qword [r11 + CTX_RIP]

.build_gpr_frame:
    ; Match timer_isr's push order exactly. After these pushes RSP points to
    ; saved R15, which matches the pop sequence below and trap_frame layout.
    push qword [r11 + CTX_RAX]
    push qword [r11 + CTX_RBX]
    push qword [r11 + CTX_RCX]
    push qword [r11 + CTX_RDX]
    push qword [r11 + CTX_RSI]
    push qword [r11 + CTX_RDI]
    push qword [r11 + CTX_RBP]
    push qword [r11 + CTX_R8]
    push qword [r11 + CTX_R9]
    push qword [r11 + CTX_R10]
    push qword [r11 + CTX_R11]
    push qword [r11 + CTX_R12]
    push qword [r11 + CTX_R13]
    push qword [r11 + CTX_R14]
    push qword [r11 + CTX_R15]

    ; Read the destination CR3 before switching address spaces. After CR3 is
    ; changed, no further access to the context pointer is allowed.
    mov rax, [r11 + CTX_CR3]
    test rax, rax
    jz .invalid_context

    mov rdx, cr3
    cmp rdx, rax
    je .restore_gprs
    mov cr3, rax

.restore_gprs:
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

    iretq

.invalid_context:
    ; A restore operation cannot return an error to C. Stop deterministically
    ; instead of continuing with a null stack or address-space root.
    ud2
